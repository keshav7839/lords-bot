/*
 * Wave B / C / D automations.
 *
 * Wave B : building upgrade, research, shelter, quests, VIP, arena.
 * Wave C : map scanning (gather / monster hunt scaffolding).
 * Wave D : traps, pets, reward claiming.
 *
 * All switches live in c->wave (wave.* config keys) and are OFF by
 * default.  Every packet sent by this module is logged so the dashboard
 * log shows exactly what fired.  Request payloads that could not be
 * taken from verified senders are marked BETA and are validated live
 * with `$probe` + `wave.log_packets` (server answers REQUEST+1 on
 * success, _MSG_RESP_BUILDINGERROR / silence otherwise).
 */

#include "waves.h"
#include "connection.h"
#include "protocol.h"
#include "net_rw.h"
#include "packet_enum.h"
#include "log.h"
#include "items.h"
#include "tech_research.h"
#include "map_point.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Building operation types (from game protocol: kBuild=1, kUpgrade=2). */
#define BUILD_OP_BUILD    1
#define BUILD_OP_UPGRADE  2

#define TECH_MAX_LEVEL 10

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Global action pacing.
 *
 * Every commercial bot has a "Worker Speed" gate (the reference default
 * is 1s between *every* action) and community ban data is explicit that
 * a metronomic action cadence is a fingerprint. This is that gate for
 * feature sends: a minimum gap plus jitter, so the interval is never
 * identical twice running.
 *
 * Only feature packets go through here. Heartbeats, login and the recv
 * path are untouched, so session health is never risked by pacing.
 *
 * A skipped send is safe because every wave tick is level-triggered —
 * it re-evaluates world state each pass rather than acting once on an
 * edge, so the next pass simply re-issues the decision.
 */
static time_t g_last_action = 0;
static uint32_t g_pace_skipped = 0;

static void SendRaw(Connection *c, uint16_t opcode,
                    const uint8_t *payload, uint16_t len)
{
	uint32_t gap = c->wave.action_min_gap_ms;
	if (gap > 0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now_ms = (int64_t)ts.tv_sec * 1000 +
		                 ts.tv_nsec / 1000000;
		if (g_last_action) {
			uint32_t jitter = (uint32_t)((now_ms * 7919u) %
			                             (gap + 1));
			uint32_t need = gap - (gap / 4) + jitter;   /* 75-100% */
			if ((uint64_t)(now_ms - g_last_action) < need) {
				g_pace_skipped++;
				if (g_pace_skipped == 1 || g_pace_skipped % 50 == 0)
					LOGI("[PACE] held back (min %ums, %u "
					     "held)\n", gap, g_pace_skipped);
				return;
			}
		}
		g_last_action = now_ms;
	}

	c->size = 2;
	write_u16(c->data + c->size, opcode);
	c->size += 2;
	write_u32(c->data + c->size, ++c->protocol.seq_id);
	c->size += 4;
	if (len && payload) {
		memcpy(c->data + c->size, payload, len);
		c->size += len;
	}
	write_u16(c->data, c->size);
	send_packet(c, true);
}

static void HexPreview(const char *tag, const uint8_t *d, uint16_t size,
                       uint16_t max_bytes)
{
	char hex[3 * 640 + 8];
	uint16_t n = size < max_bytes ? size : max_bytes;
	uint16_t i, pos = 0;

	for (i = 0; i < n && pos + 3 < sizeof(hex); i++)
		pos += (uint16_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", d[i]);

	LOGI("[WAVE] %s size=%u: %s%s\n", tag, size, hex,
	     n < size ? "..." : "");
}

void WaveRecvDump(Connection *c, const char *tag,
                  const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview(tag, data, size, 640);
}


/* Max plausible BUILDINGEVENT "position" (building slot index). The
 * unverified offset guess used to yield 3574, which is not a slot, and
 * the resulting phantom "busy" state blocked every build forever. */
#define BUILD_QUEUE_MAX_POSITION 64

static time_t g_build_busy_warned = 0;
static time_t g_build_nowarn = 0;
static time_t g_help_last = 0;
static bool g_build_decided = false;

static bool queue_idle(const BuildQueueInfo *q, int64_t server_time)
{
	if (q->total_time == 0)
		return true;
	/* The old "position 3574 must be a mis-parse" theory was wrong.
	 * A live capture (run56) confirms this layout is correct: offset 6
	 * is a real start time (2026-09-29 00:54:27 UTC) and offset 14 a
	 * 45857s duration, i.e. a genuine 12h07m build running until 13:38.
	 * `position` is a building position id and large values are normal.
	 * So the queue really was busy for hours — the actual gap was that
	 * building data never arrived at all (see the no-building-data path
	 * in WaveUpgradeOne), not a bad offset. */
	return (int64_t)(q->start_time + (int64_t)q->total_time) <= server_time;
}

/* c->technology.finish_time holds the research START instant (see
 * RecvTechnologyInfo / WaveNotifyResearchStarted), NOT the end time.
 * Comparing it directly against server_time made every research look
 * finished the moment it started, so ResearchTick re-sent a start
 * request every 45s and the server rejected all of them (err=1).
 * Mirror queue_idle(): idle only once start + total has elapsed. */
static bool research_idle(const TechnologyInfo *t, int64_t server_time)
{
	if (t->total_time == 0)
		return true;
	return (int64_t)(t->finish_time + (int64_t)t->total_time) <= server_time;
}

/* ------------------------------------------------------------------ */
/* Wave B: automatic building upgrade                                 */
/* ------------------------------------------------------------------ */

static time_t g_build_last = 0;
static time_t g_build_err_logged = 0;
static uint8_t g_build_cursor = 0;
static bool    g_build_pending = false; /* waiting for BUILDBEGIN/ERROR */

/* Upgrade order, highest value first.
 *
 * Follows the reference bot's documented order (Castle -> Resource ->
 * Academy -> Manor -> Barracks/Infirmary -> Monsterhold -> Mystic Spire
 * -> Trading Post -> Resource-no-manor -> Treasure Trove -> Workshop),
 * with the ids 18-23 that the previous enum did not name - they were
 * rejected by IsBuilding() and so were never upgraded at all.
 *
 * Notes on the tail: Prison/Altar/Battle Hall are military unlocks
 * behind very long chains, so they sit low and the scheduler will only
 * reach them once everything above is capped. */
static const uint16_t default_build_priority[] = {
	BUILD_CASTLE, BUILD_ACADEMY, BUILD_MANOR, BUILD_BARRACKS,
	BUILD_INFIRMARY, BUILD_MONSTERHOLD, BUILD_MYSTIC_SPIRE,
	BUILD_TRADING_POST, BUILD_WALL, BUILD_WATCHTOWER, BUILD_EMBASSY,
	BUILD_WORKSHOP, BUILD_VAULT, BUILD_GOLEM_TOWER, BUILD_SANCTUARY,
	BUILD_TIMBER, BUILD_STONE, BUILD_ORE, BUILD_FOOD,
	BUILD_ALTAR, BUILD_PRISON, BUILD_BATTLE_HALL
};

void WaveUpgradeOne(Connection *c)
{
	time_t now = time(NULL);

	if (c->server_time == 0)
		return;
	if (c->building_count == 0) {
		if (!g_build_nowarn || (now - g_build_nowarn) > 600) {
			g_build_nowarn = now;
			LOGI("[BUILD] no building data yet, waiting\n");
		}
		return;
	}
	if (!queue_idle(&c->build_queue, (int64_t)c->server_time)) {
		/* Throttle: this path used to return without touching
		 * g_build_last, so it logged on every 10s WaveTick (360
		 * identical lines per run). */
		if (!g_build_busy_warned || (now - g_build_busy_warned) > 600) {
			g_build_busy_warned = now;
			LOGI("[BUILD] queue busy (pos %u) until %llds\n",
			     c->build_queue.position,
			     (long long)(c->build_queue.start_time +
			                 (int64_t)c->build_queue.total_time));
		}
		g_build_last = now;
		return;
	}
	if (g_build_pending) {
		LOGI("[BUILD] previous upgrade still awaiting response\n");
		return;
	}

	const uint16_t *prio = c->wave.build_priority;
	uint8_t prio_count = c->wave.build_priority_count;

	if (prio_count == 0) {
		prio = default_build_priority;
		prio_count = (uint8_t)(sizeof(default_build_priority) /
		                       sizeof(default_build_priority[0]));
	}

	uint8_t max_level = c->wave.build_max_level ? c->wave.build_max_level : 25;

	/* One-shot decision trace: the build path is silent on the idle
	 * branch, so "no upgrade" is indistinguishable from "no target
	 * found" without it. */
	if (!g_build_decided) {
		g_build_decided = true;
		LOGI("[BUILD] deciding: %u buildings, %u priority targets, "
		     "max_level=%u, queue free\n",
		     c->building_count, prio_count, max_level);
	}

	/* Rotate through the priority list using g_build_cursor so a
	 * building rejected by the server (prereq / resources) does not
	 * block the queue forever. */
	for (uint8_t n = 0; n < prio_count; n++) {
		uint8_t idx = (uint8_t)((g_build_cursor + n) % prio_count);
		uint16_t want = prio[idx];

		for (uint16_t i = 0; i < c->building_count; i++) {
			BuildingInfo *b = &c->building[i];

			if (b->build_id != want)
				continue;
			if (b->level >= max_level)
				continue;
			if (!IsBuilding(b->build_id))
				continue;

			SendStartBuilding(c, b->position_id, b->build_id,
			                  BUILD_OP_UPGRADE);
			g_build_cursor = (uint8_t)((idx + 1) % prio_count);
			g_build_last = now;
			g_build_pending = true;
			LOGI("[BUILD] upgrade %s (id=%u) pos=%u level=%u -> %u\n",
			     GetBuildingName(b->build_id), b->build_id,
			     b->position_id, b->level, b->level + 1);
			return;
		}
	}

	if (now - g_build_err_logged > 600) {
		g_build_err_logged = now;
		LOGI("[BUILD] nothing to upgrade in priority list (max=%u)\n",
		     max_level);
	}
}

void WaveNotifyBuildStarted(Connection *c)
{
	(void)c;
	if (g_build_pending)
		LOGI("[BUILD] server accepted upgrade (BUILDBEGIN ok)\n");
	g_build_pending = false;
	g_build_last = time(NULL);
}

void WaveNotifyBuildError(Connection *c, uint8_t code)
{
	(void)c;
	LOGI("[BUILD] server rejected upgrade (error=%u), rotating list\n",
	     code);
	g_build_pending = false;
	g_build_cursor++;
	g_build_last = time(NULL);
}

static void BuildTick(Connection *c)
{
	if (!c->wave.build_upgrade)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* Wait for the pending response before trying again. */
	if (g_build_pending) {
		if (now - g_build_last > 30) {
			LOGI("[BUILD] upgrade response timeout, retrying\n");
			g_build_pending = false;
		} else {
			return;
		}
	}

	/* Ask the guild for help on whatever we just started.
	 * 1 help = -1% of the remaining timer, up to 30 per project, free.
	 * Throttled so a rejected request cannot spam the server.
	 *
	 * GATED ON HAVING A GUILD: verified by bisection that sending 2852
	 * from a guildless account makes the server close the session
	 * within ~2 minutes (a build without it runs 60+ min clean). A
	 * guildless player has nobody to help, so the request is
	 * meaningless anyway. */
	/* BUG: this used to `return` when the account has no guild, which
	 * aborted the rest of BuildTick — so on a guildless account NO
	 * building upgrade was ever attempted, silently. The guild check
	 * belongs around the help request only, not around the whole tick. */
	if (c->alliance.request_own_help && c->RoleAlliance.Channel != 0) {
		if (!g_help_last || (now - g_help_last) >= 10) {
			g_help_last = now;
			RequestRequestOwnHelp(c);
			LOGI("[HELP] requested guild help for our build\n");
		}
	}

	/* After a successful start, wait for the queue to finish; when the
	 * queue is idle re-check at a relaxed cadence. */
	if (g_build_last && (now - g_build_last) < 45)
		return;

	WaveUpgradeOne(c);
}

/* ------------------------------------------------------------------ */
/* Wave B: automatic research                                          */
/* ------------------------------------------------------------------ */

static time_t g_research_last = 0;
static bool    g_research_pending = false;
static uint16_t g_research_cursor = 0;
static bool    g_research_running = false; /* accepted — await 3208 */
static time_t  g_research_started = 0;

static const uint16_t default_research_priority[] = {
	14,                         /* proven accepted (lv 4)      */
	15,                         /* proven accepted (lv 3)      */
	16,                         /* proven accepted (lv 2)      */
	10, 11, 12, 13,             /* err2 group — retry after
	                               14/15/16 prereq bump        */
	17, 18, 20, 21, 22, 23, 24, 25,
	TECH_CONSTRUCTION_SPEED,   /* 6   */
	TECH_GEM_HARVESTING_I,     /* 8   */
	TECH_ENERGY_RECOVERY_I,    /* 74  */
	TECH_MORE_GATHERERS,       /* 123 */
	TECH_BIGGER_BAGS_I,        /* 125 */
	TECH_GOLD_STORAGE_I,       /* 143 */
	TECH_GEM_HARVESTING_II,    /* 229 */
	TECH_BIGGER_BAGS_II        /* 234 */
};

void WaveResearchOne(Connection *c)
{
	time_t now = time(NULL);

	if (c->server_time == 0)
		return;
	if (!research_idle(&c->technology, (int64_t)c->server_time)) {
		LOGI("[RESEARCH] queue busy (tech=%u)\n",
		     c->technology.research_tech);
		return;
	}
	if (g_research_pending) {
		LOGI("[RESEARCH] previous start still awaiting response\n");
		return;
	}
	if (g_research_running)
		return;

	static uint16_t auto_prio[sizeof(default_research_priority) / 2 + 400];
	static uint16_t auto_prio_n = 0;

	const uint16_t *prio = c->wave.research_priority;
	uint16_t prio_count = c->wave.research_priority_count;

	if (prio_count == 0) {
		if (auto_prio_n == 0) {
			/* curated list first, then every tech id 1..400 —
			 * covers T5-unlock researches as they open up */
			for (uint16_t k = 0; k < sizeof(default_research_priority) / 2; k++)
				auto_prio[auto_prio_n++] = default_research_priority[k];
			for (uint16_t id = 1; id <= 400; id++)
				auto_prio[auto_prio_n++] = id;
		}
		prio = auto_prio;
		prio_count = auto_prio_n;
	}

	/* The first tick can fire before the login RESEARCHINFO lands —
	 * tech_data would be all-zero and every level would read as 0. */
	{
		bool empty = true;
		for (int k = 0; k < (int)sizeof(c->technology.tech_data); k++) {
			if (c->technology.tech_data[k]) {
				empty = false;
				break;
			}
		}
		if (empty)
			return;
	}

	for (uint16_t n = 0; n < prio_count; n++) {
		uint16_t i = (uint16_t)((g_research_cursor + n) % prio_count);
		uint16_t tech = prio[i];
		const TechInfo *info = GetTechInfo((TechnologyId)tech);

		if (tech == 0)
			continue;

		/* Skip techs we already maxed (levels packed in tech_data). */
		uint8_t level = GetTechLevel(c->technology.tech_data, tech);
		if (level >= TECH_MAX_LEVEL)
			continue;
		(void)info;

		/* Game-verified payload (DataManager.sendTechnologyResearchStart):
		 *   u16 TechID | i32 (currentLevel + 1) */
		uint8_t payload[6];
		write_u16(payload, tech);
		write_u32(payload + 2, (uint32_t)(level + 1));
		SendRaw(c, _MSG_REQUEST_RESEARCH_EVENT_START, payload, 6);

		g_research_cursor = (uint16_t)((i + 1) % prio_count);
		g_research_last = now;
		g_research_pending = true;
		LOGI("[RESEARCH] start tech=%u level=%u (u16+i32 game payload)\n",
		     tech, level + 1);
		return;
	}

	if (now - g_research_last > 600) {
		g_research_last = now;
		LOGI("[RESEARCH] nothing to research in priority list\n");
	}
}

void WaveNotifyResearchStarted(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size > 0)
		HexPreview("RESEARCH_START_RESP", data, size, 64);
	if (g_research_pending) {
		if (size == 0) {
			LOGI("[RESEARCH] empty 3203\n");
		} else if (data[0] == 0 && size >= 16) {
			/* RecvTechnologyResearch success layout:
			 *   u8 err=0 | u16 tech | u8 unk | i64 start | u32 total
			 *   | 5×u32 rss | u32 petRss  — store the queue so
			 *   ResearchTick waits for completion. */
			c->technology.research_tech = read_u16(data + 1);
			c->technology.unk            = data[3];
			c->technology.finish_time    = read_i64(data + 4);
			c->technology.total_time     = read_u32(data + 12);
			g_research_running = true;
			g_research_started = time(NULL);
			LOGI("[RESEARCH] START ACCEPTED tech=%u level=%u — "
			     "queue running\n", c->technology.research_tech,
			     c->technology.unk);
		} else if (data[0] != 0) {
			LOGI("[RESEARCH] server rejected start (error=%u) — "
			     "rotating priority list\n", data[0]);
			g_research_cursor++;
		}
	}
	g_research_pending = false;
	g_research_last = time(NULL);
}

void WaveNotifyResearchComplete(Connection *c, const uint8_t *data,
                                uint16_t size)
{
	if (size > 0)
		HexPreview("RESEARCH_COMPLETE", data, size, 16);
	g_research_running = false;
	c->technology.research_tech = 0;
	c->technology.finish_time = 0;
	c->technology.total_time = 0;
	LOGI("[RESEARCH] complete push (3208) — queue free\n");
}

/* Arms the "research in progress" state from the login-time RESEARCHINFO
 * (or any later one). Before this existed, g_research_running was only set
 * by an accepted start response, so on every fresh login the bot believed
 * the academy was idle and fired a start request at a busy queue. */
void WaveNotifyResearchState(Connection *c)
{
	if (c->technology.total_time == 0 || c->technology.finish_time == 0) {
		g_research_running = false;
		return;
	}

	int64_t end = c->technology.finish_time + (int64_t)c->technology.total_time;
	if (end <= c->server_time) {
		g_research_running = false;
		return;
	}

	g_research_running = true;
	g_research_started = time(NULL);
	LOGI("[RESEARCH] in progress tech=%u remaining=%llds — queue busy\n",
	     c->technology.research_tech, (long long)(end - c->server_time));
}

static void ResearchTick(Connection *c)
{
	if (!c->wave.research_auto)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	if (g_research_pending) {
		if (now - g_research_last > 30) {
			LOGI("[RESEARCH] start response timeout, retrying\n");
			g_research_pending = false;
		} else {
			return;
		}
	}

	/* Started research blocks the next start until the 3208 complete
	 * push arrives (total_time in the start response is always 0, so
	 * finish_time cannot be trusted for idling). */
	if (g_research_running) {
		if (now - g_research_started > 1800) {
			LOGI("[RESEARCH] complete push never arrived — unlocking\n");
			g_research_running = false;
		} else {
			return;
		}
	}

	if (g_research_last && (now - g_research_last) < 45)
		return;

	WaveResearchOne(c);
}

/* ------------------------------------------------------------------ */
/* Wave B: shelter on incoming attack/scout                            */
/* ------------------------------------------------------------------ */

static time_t g_shelter_last = 0;

void WaveShelterRequest(Connection *c, bool from_attack)
{
	const char *why = from_attack ? "incoming attack" : "incoming scout";

	if (!c->wave.shelter_on_attack && !c->wave.shelter_on_scout)
		return;
	if (from_attack && !c->wave.shelter_on_attack)
		return;
	if (!from_attack && !c->wave.shelter_on_scout)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_shelter_last && (now - g_shelter_last) < 60)
		return;
	g_shelter_last = now;

	/* Game payload (HideArmyManager.SendHideTroopInshelter):
	 *   u8 HideLord | u8 TimeIndex | u16 mask
	 *   | for each set bit i (ascending): u32 count_i
	 * Counts appended CONTIGUOUSLY for nonzero types in bit order —
	 * bit i = m_Soldier[i] = kind*4+tier; 2401 order = inf, ranged,
	 * cav, siege. T5 unlocks types 16-19 → mask widens to u32 there
	 * (2.201 layout unverified — first T5 shelter gets probed). */
	uint32_t mask = 0;
	uint32_t counts[TROOP_MAX_TIERS * 4];
	int n = 0;
	uint8_t tiers = c->troop.tiers ? c->troop.tiers : 4;

	if (!c->troop.loaded) {
		LOGI("[SHELTER] %s — no army data yet (2401 pending), "
		     "skipping\n", why);
		return;
	}

	uint32_t *base[4] = {
		c->troop.infantry, c->troop.ranged,
		c->troop.cavalry, c->troop.siege
	};
	for (int k = 0; k < 4; k++)
		for (int t = 0; t < tiers && t < TROOP_MAX_TIERS; t++)
			if (base[k][t]) {
				mask |= 1u << (k * 4 + t);
				counts[n++] = base[k][t];
			}

	if (mask == 0) {
		LOGI("[SHELTER] %s — no troops in garrison, nothing to hide\n",
		     why);
		return;
	}

	uint8_t payload[6 + TROOP_MAX_TIERS * 16];
	int mask_bytes = (mask > 0xFFFFu) ? 4 : 2;
	payload[0] = 0;          /* hide lord=0 — hideLord=1 → LORDERR   */
	payload[1] = 0;          /* TimeIndex / rally countdown slot      */
	if (mask_bytes == 4)
		write_u32(payload + 2, mask);
	else
		write_u16(payload + 2, (uint16_t)mask);
	int hdr = 2 + mask_bytes;
	for (int k = 0; k < n; k++)
		write_u32(payload + hdr + k * 4, counts[k]);
	SendRaw(c, _MSG_REQUEST_HIDETROOPINSHELTER, payload,
	        (uint16_t)(hdr + n * 4));
	LOGI("[SHELTER] %s — mask=0x%X maskb=%d types=%d total=%u tiers=%u\n",
	     why, mask, mask_bytes, n, c->troop.total,
	     (unsigned)c->troop.tiers);
}

void WaveRecvShelterData(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview("SHELTER_DATA", data, size, 48);

	/* HideArmyManager.RecvShelterData (5601):
	 *   u16 LordID | i64 begin | u32 require | u16 mask
	 *   | for each set bit i (ascending): u32 count_i
	 * (same compact-count encoding as 5602). */
	if (size < 16) {
		LOGI("[SHELTER] data short (%u)\n", size);
		return;
	}
	uint16_t lord   = read_u16(data);
	int64_t  begin  = read_i64(data + 2);
	uint32_t req    = read_u32(data + 10);
	uint16_t mask   = read_u16(data + 14);

	c->shelter_begin    = begin;
	c->shelter_require  = req;
	c->shelter_mask     = mask;

	int n = 0;
	for (int i = 0; i < 20; i++)
		if (mask & (1u << i))
			n++;

	int hdr = 16;
	if (hdr + n * 4 != size) {
		LOGI("[SHELTER] hidden: lord=%u begin=%lld require=%u "
		     "mask=0x%X types=%d (count block %d != %u — layout guess)\n",
		     lord, (long long)begin, req, mask, n, hdr + n * 4, size);
		return;
	}

	LOGI("[SHELTER] hidden: lord=%u begin=%lld require=%u mask=0x%X\n",
	     lord, (long long)begin, req, mask);
	int off = 16;
	int total = 0;
	for (int i = 0; i < 20; i++) {
		if (!(mask & (1u << i)))
			continue;
		uint32_t cnt = read_u32(data + off); off += 4;
		total += (int)cnt;
		LOGI("[SHELTER]   type %2d = %u\n", i, cnt);
	}
	LOGI("[SHELTER] total hidden troops: %d\n", total);
}

void WaveRecvShelter(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview("SHELTER_RESP", data, size, 32);
	if (size >= 1) {
		if (data[0] == 0)
			LOGI("[SHELTER] success (troops in shelter)\n");
		else
			LOGI("[SHELTER] result=%u (0=ok)\n", data[0]);
	}
}

/* ------------------------------------------------------------------ */
/* Wave B: quest claiming                                              */
/*                                                                      */
/* Payloads taken from the game client (MissionManager):               */
/*   MISSION_START   (3113): u8 (group+1) | u8 (slotIndex+1)          */
/*   MISSION_FINISH  (3117): u8 (group+1) | u8 (slotIndex+1)          */
/*     ^ slots are 1-BASED on the wire — game sends                  */
/*       BtnID3+1 (AffairMission.cs:257/269); byte0 = no response.   */
/*   MISSION_COMPLETE(3119): u16 recordID                              */
/*   MISSIONINFO req (3111): u8 (group+1)   group 0=affair 1=alliance  */
/*   MISSIONINFO rsp (3112): u8 flag(1=reset-ack)                      */
/*                           else u8 (group+1) | i64 reset | i64 time  */
/*                                 | u8 count | count×{u16 id,u16 q,   */
/*                                   u16 base,u16 item,u8 state}       */
/*   state: 0=Wait 1=Complete 2=Reward 3=Countdown 4=AutoComplete      */
/* ------------------------------------------------------------------ */

#define Q_STATE_WAIT         0
#define Q_STATE_COMPLETE     1
#define Q_STATE_REWARD       2
#define Q_STATE_COUNTDOWN    3
#define Q_STATE_AUTOCOMPLETE 4
#define Q_SLOTS  8
#define Q_GROUPS 2

typedef struct {
	uint8_t  have;
	uint8_t  count;
	int64_t  reset_time;
	int64_t  mission_time;
	uint16_t id[Q_SLOTS];
	uint8_t  state[Q_SLOTS];
} QuestGroup;

static QuestGroup g_qgroup[Q_GROUPS];
static time_t     g_qslot_sent[Q_GROUPS][Q_SLOTS];
static time_t     g_quest_last = 0;
static time_t     g_qinfo_ping_at = 0; /* deferred flag=1 re-request */
static uint16_t   g_claim_cursor = 1;
static uint8_t    g_qflag[256];
static uint16_t   g_qflag_len = 0;

static const char *q_state_name(uint8_t s)
{
	switch (s) {
	case Q_STATE_WAIT:         return "Wait";
	case Q_STATE_COMPLETE:     return "Complete";
	case Q_STATE_REWARD:       return "Reward";
	case Q_STATE_COUNTDOWN:    return "Countdown";
	case Q_STATE_AUTOCOMPLETE: return "AutoComplete";
	default:                   return "?";
	}
}

/* Fire START/FINISH for one slot using the game's exact 2-byte payload. */
static void q_fire(Connection *c, uint8_t group, uint8_t slot, uint16_t op)
{
	uint8_t payload[2];
	payload[0] = (uint8_t)(group + 1);
	/* 1-based slot on the wire (game: BtnID3+1, AffairMission.cs:257/269).
	 * Verified live: 3117 + byte0 → silent drop; byte1 → 45-byte OK. */
	payload[1] = (uint8_t)(slot + 1);
	SendRaw(c, op, payload, 2);
}

void WaveRecvMissionInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	time_t now = time(NULL);

	HexPreview("MISSIONINFO", data, size, 96);

	if (size < 1)
		return;

	/* v1.80 game: flag=1 → ignore + re-request in 2s. But this 2.201
	 * server answers EVERY poll with flag=1 carrying a FULL valid
	 * 91-byte body (fresh states — e.g. Reward transitions appear
	 * there). Parse whenever the structure checks out; keep the 3s
	 * re-request stream so states stay live (matches the game's own
	 * 2s loop — the server expects it). */
	uint8_t grp1;
	if (data[0] == 1) {
		if (size < 19) {
			LOGI("[QUEST] mission info ping (%u bytes) — re-request 3s\n",
			     size);
			if (c->wave.quest_claim)
				g_qinfo_ping_at = now + 3;
			return;
		}
		grp1 = data[1];
		if (grp1 < 1 || grp1 > Q_GROUPS ||
		    data[18] == 0 || data[18] > Q_SLOTS ||
		    (uint16_t)(19 + data[18] * 9) > size) {
			LOGI("[QUEST] ping body invalid — re-request 3s\n");
			if (c->wave.quest_claim)
				g_qinfo_ping_at = now + 3;
			return;
		}
		LOGI("[QUEST] flag=1 carries fresh data — parsing\n");
		if (c->wave.quest_claim)
			g_qinfo_ping_at = now + 3;
	} else if (size < 19) {
		LOGI("[QUEST] mission info short (%u bytes) — unparsed\n", size);
		return;
	} else {
		grp1 = data[1];
		if (grp1 < 1 || grp1 > Q_GROUPS) {
			LOGI("[QUEST] mission info unknown group %u\n", grp1);
			return;
		}
	}
	QuestGroup *g = &g_qgroup[grp1 - 1];

	uint16_t old_id[Q_SLOTS];
	uint8_t  old_state[Q_SLOTS], old_count = g->count;
	memcpy(old_id, g->id, sizeof(old_id));
	memcpy(old_state, g->state, sizeof(old_state));

	g->have = 1;
	g->reset_time   = read_i64(data + 2);
	g->mission_time = read_i64(data + 10);
	g->count = data[18];
	if (g->count > Q_SLOTS)
		g->count = Q_SLOTS;

	uint16_t off = 19;
	for (uint8_t i = 0; i < g->count; i++) {
		if (off + 9 > size)
			break;
		g->id[i]    = read_u16(data + off); off += 2;
		/* quality = u16 */  off += 2;
		/* base    = u16 */  off += 2;
		/* itemID  = u16 */  off += 2;
		g->state[i] = data[off]; off += 1;
	}

	bool changed = (old_count != g->count);
	for (uint8_t i = 0; i < g->count && !changed; i++)
		if (old_id[i] != g->id[i] || old_state[i] != g->state[i])
			changed = true;
	if (changed)
		LOGI("[QUEST] group %u (%s): %u mission(s) reset=%lld"
		     " time=%lld%s\n",
		     grp1 - 1, grp1 == 1 ? "affair" : "alliance", g->count,
		     (long long)g->reset_time, (long long)g->mission_time,
		     data[0] == 1 ? " (flag=1 body)" : "");

	for (uint8_t i = 0; i < g->count; i++) {
		if (changed)
			LOGI("[QUEST]   slot %u id=%u state=%s\n",
			     i, g->id[i], q_state_name(g->state[i]));

		if (!c->wave.quest_claim)
			continue;
		if (g_qslot_sent[grp1 - 1][i] &&
		    now - g_qslot_sent[grp1 - 1][i] < 300)
			continue;

		if (g->state[i] == Q_STATE_WAIT) {
			q_fire(c, (uint8_t)(grp1 - 1), i,
			       _MSG_REQUEST_MISSION_START);
			g_qslot_sent[grp1 - 1][i] = now;
			LOGI("[QUEST] START sent (group %u slot %u)\n",
			     grp1 - 1, i);
		} else if (g->state[i] == Q_STATE_REWARD ||
		           g->state[i] == Q_STATE_AUTOCOMPLETE) {
			/* Game only offers FINISH (BtnID1=7) for Reward and
			 * AutoComplete (AffairMission.cs switch); Complete means
			 * already claimed — server rejects a second FINISH. */
			q_fire(c, (uint8_t)(grp1 - 1), i,
			       _MSG_REQUEST_MISSION_FINISH);
			g_qslot_sent[grp1 - 1][i] = now;
			LOGI("[QUEST] FINISH sent (group %u slot %u state=%s)\n",
			     grp1 - 1, i, q_state_name(g->state[i]));
		}
	}
}

/* MISSION_FLAG: u16 byteLen | bitfield.  bit i of byte b = record key
 * (b*8 + i + 1) already claimed (game: SetBoolMark does ID-1 >> 3). */
void WaveRecvMissionFlag(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size < 2) {
		WaveRecvDump(c, "MISSION_FLAG", data, size);
		return;
	}
	uint16_t len = read_u16(data);
	if (len > sizeof(g_qflag))
		len = sizeof(g_qflag);
	if ((uint32_t)len + 2 > size)
		len = (uint16_t)(size - 2);
	memcpy(g_qflag, data + 2, len);
	g_qflag_len = len;

	uint32_t set = 0;
	for (uint16_t b = 0; b < len; b++)
		for (int k = 0; k < 8; k++)
			if (g_qflag[b] & (1 << k))
				set++;
	LOGI("[QUEST] flag: %u turf quest(s) claimed (bitfield %u bytes)\n",
	     set, len);
}

/* 3114 / 3118: u8 (group+1) | u8 (slot+1; 0 = rejected) | ...
 * 3120:         u16 recordID (0 = failed). */
void WaveRecvMissionResult(Connection *c, const char *tag,
                           const uint8_t *data, uint16_t size)
{
	HexPreview(tag, data, size, 48);

	if (strcmp(tag, "MISSION_COMPLETE_RESP") == 0) {
		uint16_t id = size >= 2 ? read_u16(data) : 0;
		if (id != 0)
			LOGI("[QUEST] turf quest CLAIMED id=%u\n", id);
		(void)c;
		return;
	}

	if (size >= 2) {
		if (data[1] == 0) {
			LOGI("[QUEST] %s: rejected (group byte %u)\n",
			     tag, data[0]);
		} else {
			LOGI("[QUEST] %s: OK group=%u slot=%u\n", tag,
			     data[0] - 1, data[1] - 1);
		}
	} else if (size >= 1) {
		LOGI("[QUEST] %s first byte=%u\n", tag, data[0]);
	}
	(void)c;
}

static void QuestTick(Connection *c)
{
	if (!c->wave.quest_claim)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* Deferred flag=1 refresh — fire ~3s after the ping arrives. */
	if (g_qinfo_ping_at && now >= g_qinfo_ping_at) {
		g_qinfo_ping_at = 0;
		RequestMissionInfo(c, 0);
		RequestMissionInfo(c, 1);
		LOGI("[QUEST] delayed ping re-request sent\n");
	}

	if (g_quest_last && (now - g_quest_last) < 300)
		return;
	g_quest_last = now;

	/* Refresh both timed-mission groups; start/finish actions fire in
	 * WaveRecvMissionInfo when the fresh state arrives. */
	RequestMissionInfo(c, 0);
	RequestMissionInfo(c, 1);

	/* Daily mission reward — VALIDATED live: empty payload answers
	 * _MSG_RESP_DAILY_MISSION_REWARD (11873) size=12. */
	SendRaw(c, _MSG_REQUEST_DAILY_MISSION_REWARD, NULL, 0);

	/* Turf-quest claim scan: MISSION_COMPLETE wants a record ID whose
	 * condition we cannot evaluate client-side, so sweep IDs — the
	 * server only answers 3120 (with ID != 0) when it really claims. */
	uint16_t first = g_claim_cursor;
	for (int k = 0; k < 6; k++) {
		uint8_t payload[2];
		write_u16(payload, g_claim_cursor);
		SendRaw(c, _MSG_REQUEST_MISSION_COMPLETE, payload, 2);
		if (++g_claim_cursor > 600)
			g_claim_cursor = 1;
	}
	LOGI("[QUEST] info requested (groups 0+1); daily reward sent; "
	     "claim scan ids %u..%u\n", first,
	     first + 5 <= 600 ? first + 5 : 600);
}

/* ------------------------------------------------------------------ */
/* Wave B: VIP chest                                                   */
/* ------------------------------------------------------------------ */

static time_t g_vip_last = 0;

static void VipTick(Connection *c)
{
	if (!c->wave.vip_collect)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_vip_last && (now - g_vip_last) < 3600)
		return;
	g_vip_last = now;

	/* VALIDATED payload: empty — answered by _MSG_RESP_MISSION_VIP
	 * (3125) live; errors are harmless when the chest is not ready. */
	SendRaw(c, _MSG_REQUEST_MISSION_VIP_COLLECT, NULL, 0);
	LOGI("[VIP] VIP chest collect attempted (validated)\n");
}

/* ------------------------------------------------------------------ */
/* Wave B: arena                                                       */
/*                                                                      */
/* Payloads from ArenaManager (game client):                            */
/*   ARENA_INFO (5201, server push):                                   */
/*     u32 myPlace | 5×u16 defHeroes | u8 todayChal | u8 todayReset    */
/*     | i64 lastChal | u32 crystal | 3×target | ...                   */
/*     target = u16 head | char[13] name | char[3] tag | u32 place     */
/*              | 5×{u16 id, u8 lvl, u8 rank, u8 star, u8 equip}      */
/*   ARENA_REFRESH_TARGET req (5205): u8 kind                          */
/*   ARENA_REFRESH_TARGET rsp (5206): u8 kind | u8 err | 3×target      */
/*   ARENA_CHALLENGE req (5208):                                      */
/*     u8 targetIdx | u32 place | char[13] name | 5×u16 myHeroes       */
/*   ARENA_BOARDDATA req (4605): empty (ladder view only)              */
/* ------------------------------------------------------------------ */

static time_t g_arena_last = 0;        /* last challenge sent */
static time_t g_arena_refresh_last = 0;

static struct {
	uint8_t  have_info;      /* parsed a 5201 push */
	uint8_t  have_target;    /* target[0] known */
	uint32_t place;          /* my rank */
	uint16_t def_hero[5];    /* my defense squad = challenge squad */
	uint32_t t_place;        /* target rank */
	uint8_t  t_name[13];     /* target name (raw 13 bytes) */
} g_arena;

static void arena_store_target(const uint8_t *data, uint16_t off,
                               uint16_t size)
{
	/* off points at u16 head */
	if (off + 52 > size)
		return;
	g_arena.t_place = read_u32(data + off + 18); /* 2+13+3 = 18 */
	memcpy(g_arena.t_name, data + off + 2, 13);
	g_arena.have_target = 1;

	char name[14];
	memcpy(name, data + off + 2, 13);
	name[13] = 0;
	LOGI("[ARENA] target0: rank=%u name=%s\n",
	     g_arena.t_place, name);
}

void WaveRecvArenaBoard(Connection *c, const uint8_t *data, uint16_t size)
{
	/* Ladder view only — challenge flow uses 5201/5206 targets. */
	WaveRecvDump(c, "ARENA_BOARD", data, size);
}

void WaveRecvArenaInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("ARENA_INFO", data, size, 96);

	if (size < 80) { /* 28 header + 52-byte target0 minimum */
		LOGI("[ARENA] info short (%u bytes, want >= 80)\n", size);
		return;
	}

	g_arena.place     = read_u32(data);
	for (int i = 0; i < 5; i++)
		g_arena.def_hero[i] = read_u16(data + 4 + i * 2);
	g_arena.have_info = 1;

	LOGI("[ARENA] my rank=%u def heroes=[%u %u %u %u %u]\n",
	     g_arena.place, g_arena.def_hero[0], g_arena.def_hero[1],
	     g_arena.def_hero[2], g_arena.def_hero[3], g_arena.def_hero[4]);

	/* targets start at offset 28 (4+10+1+1+8+4) */
	arena_store_target(data, 28, size);
}

void WaveRecvArenaRefresh(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("ARENA_REFRESH", data, size, 64);

	if (size < 4) {
		LOGI("[ARENA] refresh short (%u bytes)\n", size);
		return;
	}
	if (data[1] != 0) {
		LOGI("[ARENA] refresh error %u (kind=%u)\n", data[1], data[0]);
		return;
	}
	if (size < 158) { /* 2 + 3×52-byte targets */
		LOGI("[ARENA] refresh missing targets (%u bytes)\n", size);
		return;
	}
	arena_store_target(data, 2, size);
}

void WaveRecvArenaChallenge(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("ARENA_CHALLENGE_RESP", data, size, 96);
	if (size >= 1) {
		if (data[0] == 0)
			LOGI("[ARENA] battle result received (first byte 0)\n");
		else
			LOGI("[ARENA] challenge error %u\n", data[0]);
	}
	(void)c;
}

static time_t g_arena_prize_last = 0;

void WaveRecvArenaPrize(Connection *c, const uint8_t *data, uint16_t size)
{
	/* ArenaManager.RecvArena_Arena_GetPrize: u8 err | u32 newDiamond
	 * (only when err == 0). */
	if (size < 1)
		return;
	if (data[0] != 0) {
		LOGI("[ARENA] prize claim rejected (err=%u)\n", data[0]);
		return;
	}
	if (size >= 5) {
		uint32_t total = read_u32(data + 1);
		LOGI("[ARENA] gem prize claimed — diamond balance now %u\n",
		     total);
		c->player.gems = total;
	}
}

/* ------------------------------------------------------------------ */
/* Shelter upkeep                                                       */
/*                                                                      */
/* CONFIRMED against the decompiled client:                            */
/*   RELEASESHELTERTROOP req: seq, empty payload, unencrypted          */
/*     (HideArmyManager.SendReleaseShelterTroop)                        */
/*   RecvShelterData: u16 lordId | ... (begin/require time)            */
/*                                                                      */
/* The reference bot keeps shelter on a ~2h re-shelter cycle and        */
/* releases after an attacker finishes. Without that, a 12h shelter     */
/* simply expires and the leader is exposed with no troops home — the   */
/* single largest permanent-loss risk in the protection set.            */
/* ------------------------------------------------------------------ */

static void ShelterTick(Connection *c)
{
	if (!c->wave.shelter_always)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_shelter_last && (now - g_shelter_last) < 300)
		return;
	g_shelter_last = now;

	/* Only act once we actually know the shelter window. */
	if (c->shelter_require == 0 || c->shelter_begin == 0)
		return;

	int64_t end = c->shelter_begin + (int64_t)c->shelter_require;
	int64_t remaining = end - (int64_t)c->server_time;
	if (remaining <= 0) {
		LOGI("[SHELTER] expired — leader exposed\n");
		return;
	}

	/* Re-shelter while there is still plenty of time left, so the
	 * window never lapses. */
	if (remaining < (int64_t)(2 * 3600)) {
		/* shelter_on_attack doubles as the "sheltering is allowed"
		 * switch; the tick reuses the same request path. */
		WaveShelterRequest(c, true);
		LOGI("[SHELTER] renewing, %llds left on the current window\n",
		     (long long)remaining);
	}
}

/* 1417 equipment inventory.
 *
 * CONFIRMED layout (LordEquipData.Recv_MSG_RESP_LORDEQUIP, type 0):
 *   u8 type | i64 updateTime | u16 offset | u16 count
 *   | count x 27B record
 * offset 0 with count 0 is a "clear everything" packet. Records arrive
 * in pages of up to 200, so a non-zero offset means a continuation.
 */
static uint16_t g_gear_inv[200][3];   /* itemID, color, serial(low) */
static uint16_t g_gear_inv_count = 0;
static bool     g_gear_inv_loaded = false;

void WaveRecvLordEquipInv(Connection *c, const uint8_t *data, uint16_t size)
{
	static bool dumped = false;
	if (!dumped) {
		dumped = true;
		WaveRecvDump(c, "LORDEQUIP_INV", data, size);
	}
	if (size < 14)
		return;
	uint16_t offset = 0;
	uint8_t  type   = data[offset++];
	int64_t  upd    = (int64_t)read_u64(data + offset); offset += 8;
	uint16_t start  = read_u16(data + offset); offset += 2;
	uint16_t count  = read_u16(data + offset); offset += 2;
	(void)upd;

	if (type != 0) {
		LOGI("[GEAR] inventory packet type=%u (not the list form)\n",
		     type);
		return;
	}
	if (start == 0 && count == 0) {
		g_gear_inv_count = 0;
		g_gear_inv_loaded = true;
		LOGI("[GEAR] inventory cleared by server\n");
		return;
	}

	for (uint16_t i = 0; i < count && start + i < 200; i++) {
		if (offset + 27 > size)
			break;
		uint16_t item_id = read_u16(data + offset);
		uint8_t  color   = data[offset + 2];
		/* +3 itemID+color, +4 gem colours, +8 gem ids, +4 serial */
		uint32_t serial  = read_u32(data + offset + 15);
		offset += 27;
		g_gear_inv[start + i][0] = item_id;
		g_gear_inv[start + i][1] = color;
		g_gear_inv[start + i][2] = (uint16_t)serial;
	}
	if (start + count > g_gear_inv_count)
		g_gear_inv_count = start + count;
	g_gear_inv_loaded = true;

	char line[256];
	size_t off = 0;
	for (uint16_t i = 0; i < g_gear_inv_count && off < sizeof(line) - 20; i++) {
		if (!g_gear_inv[i][0])
			continue;
		off += (size_t)snprintf(line + off, sizeof(line) - off,
		                       "%sitem%u/q%u", off ? " " : "",
		                       g_gear_inv[i][0],
		                       (unsigned)g_gear_inv[i][1]);
	}
	LOGI("[GEAR] inventory: %u entries: %s\n", g_gear_inv_count,
	     line[0] ? line : "(empty)");
}

/* 3801 TALENTINFO - the account's talent levels.
 *
 * Layout (from the live 102-byte payload, corroborated by the decompile's
 * TalentLevelTbl { ID, TalentID, Level, NeedPoint, Effect, EffectVal }):
 *   byte 0-1 : header, zero on this account
 *   byte 2   : group/kind marker (0x05 here)
 *   byte 3-47: 45 per-talent level bytes, 0 meaning not yet spent
 *   byte 48+ : further level banks, all zero here
 *
 * Only the level bank is interpreted. The header is reported but not
 * acted on, because unlike the treasure extras below there is no second
 * sample here to pin its meaning down - so it stays read-only.
 *
 * This is what makes talent automatable later: NeedPoint means talent
 * spends points rather than resources, so there is no gem or food risk,
 * and 3802 REQUEST_TALENT_LEVEL_ADD is already known to exist. What is
 * still missing is that request's body, so nothing is sent. */
void WaveRecvTalentInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	static bool dumped = false;
	if (!dumped) {
		dumped = true;
		WaveRecvDump(c, "TALENTINFO", data, size);
	}
	if (size < 3)
		return;

	/* The level bank runs to the last non-zero byte, not to the first
	 * one: a talent at level 0 is an unspent talent and appears in the
	 * middle of the bank, so "stop at the first zero" truncates after
	 * three entries on a live account. */
	uint16_t base = 3;
	uint16_t last = base;
	for (uint16_t i = base; i < size; i++)
		if (data[i] != 0)
			last = i;
	uint16_t end = last + 1;
	if (end <= base) {
		LOGI("[TALENT] no talents unlocked yet\n");
		return;
	}

	uint16_t max_lv = 0, spent = 0, owned = 0;
	for (uint16_t i = base; i < end; i++) {
		if (data[i] > max_lv) max_lv = data[i];
		spent += data[i];
		if (data[i]) owned++;
	}
	LOGI("[TALENT] %u of %u talents started, highest level %u, "
	     "%u points invested (header=%u)\n",
	     owned, end - base, max_lv, spent, data[2]);
}

/* 4026/4041/4053/4056/4068 TREASURE_LIST_EXTRA* - per-entry flag arrays.
 *
 * Layout CONFIRMED: u8 count, then exactly `count` flag bytes. Both
 * samples are 48 bytes with a leading 0x2F (47), i.e. 1 + 47, and in
 * 4056 the trailing bytes are a clean run of 0x01 - a flag array, not a
 * coincidental byte pattern.
 *
 * 4056 is the double-ticket state. This is the read-only half of the
 * treasure feature: it says which treasure entries can be run twice
 * without spending a gem, so a future claim loop can prefer them. No
 * treasure request is sent - the request bodies are still unknown, and
 * an unconfirmed request on this server costs the session. */
void WaveRecvTreasureExtra(Connection *c, const uint8_t *data, uint16_t size)
{
	static bool dumped = false;
	if (!dumped) {
		dumped = true;
		WaveRecvDump(c, "TREASURE_EXTRA", data, size);
	}
	if (size < 1)
		return;

	uint8_t count = data[0];
	if (count + 1u > size) {
		LOGI("[TREASURE] extra flags truncated: says %u, payload %u\n",
		     count, size);
		return;
	}

	uint16_t set = 0;
	for (uint8_t i = 0; i < count; i++)
		if (data[1 + i]) set++;

	LOGI("[TREASURE] %u of %u entries flagged (free double-run: %u)\n",
	     set, count, set);
}

/* Pushes the server volunteers that the bot has no reader for yet.
 *
 * The bot receives these unprompted every run and drops them, which is
 * the cheapest possible source of new capability: parsing a push costs
 * no request, so it cannot be the thing that closes the session.
 *
 * One-shot hex dumps are enough to write a reader from real bytes, the
 * same way the equipment reader was built (3804). Nothing here sends a
 * packet. */
bool WaveIsInterestingPush(uint16_t opcode)
{
	switch (opcode) {
	case 3801:   /* TALENTINFO - talent point state              */
	case 4001:   /* TREASURE_LIST - claimable treasures         */
	case 4026: case 4041: case 4053: case 4056: case 4068:
	             /* TREASURE_LIST_EXTRA*                        */
	case 2601:   /* WALLINFO - wall state beside the traps      */
	case 9402:   /* VALHALLA_INFO                               */
	case 3140:   /* VALHALLA_MISSION                            */
	case 9771:   /* RELICS_INFO                                 */
	case 9795:   /* RELICS_RANKGACHA_LIST                       */
	case 5401:   /* WONDER_INIT_NOTICE                          */
	case 1821:   /* SPCHALLENGE2_INFO                           */
	case 2063:   /* DECORATION_INFO                             */
		return true;
	default:
		return false;
	}
}

/* Mystic Spire item craft (8216 ITEMCRAFT_INFO).
 *
 * CONFIRMED request shape (PetManager.SendItemCraft_Start):
 *   seq | u16 craftID | u16 count | u8 instantComplete
 *
 * The response is variant-tagged: first byte 3 selects a different
 * parser, otherwise a second byte selects type 0/1/2 with their own
 * field sets. The account's live 20-byte payload is dumped once so the
 * branch can be identified from real data rather than assumed. */
static bool g_itemcraft_dumped = false;

void WaveRecvItemCraftInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	if (!g_itemcraft_dumped) {
		g_itemcraft_dumped = true;
		WaveRecvDump(c, "ITEMCRAFT_INFO", data, size);
	}
	if (size < 2)
		return;
	uint8_t first = data[0];
	LOGI("[SPIRE] craft info %uB variant=%u sub=%u\n",
	     size, first, data[1]);
}

/* ------------------------------------------------------------------ */
/* Wave D: leader equipment (3804 ONLORDEQUIP_INFO)                    */
/*                                                                      */
/* Layout CONFIRMED against the decompiled client                       */
/* (LordEquipData.Recv_MSG_RESP_ONLORDEQUIP_INFO): 8 slots, each        */
/*   u16 itemID | u8 color | 4x u8 gemColor | 4x u16 gemID | u32 serial */
/* = 27 bytes per slot, 216 total. The bot already receives this push   */
/* and was discarding it, so it had no idea what gear the account owns.  */
/*                                                                      */
/* Request shapes (same source, for when a swap is wired):              */
/*   3805 equip/unequip : seq | u8 equipPos | u32 serial                */
/*   3809 craft         : seq | u16 itemID  | u32 serial                */
/* ------------------------------------------------------------------ */

void WaveRecvLordEquipInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	/* 8 x 27 = 216. Tolerate a short packet rather than reading past
	 * the end of the buffer. */
	uint16_t offset = 0;
	uint16_t equipped = 0;
	char line[256];
	size_t off = 0;

	for (int slot = 0; slot < 8; slot++) {
		if (offset + 27 > size) {
			LOGI("[GEAR] info short at slot %d (%uB read of %u)\n",
			     slot, offset, size);
			break;
		}
		uint16_t item_id = read_u16(data + offset);
		uint8_t  color   = data[offset + 2];
		offset += 3;                       /* itemID + color */
		offset += 4;                       /* 4 gem colours  */
		offset += 8;                       /* 4 gem ids      */
		uint32_t serial  = read_u32(data + offset);
		offset += 4;

		if (item_id != 0) {
			equipped++;
			off += (size_t)snprintf(line + off, sizeof(line) - off,
			                       "%sslot%d=item%u/q%u/s%u",
			                       off ? " " : "", slot, item_id,
			                       (unsigned)color, serial);
		}
	}

	LOGI("[GEAR] %u of 8 slots equipped: %s\n", equipped,
	     equipped ? line : "(nothing)");
}

/* ------------------------------------------------------------------ */
/* Wave E: free Turf box (ONLINE_GIFT 1117 -> 1118)                     */
/*                                                                      */
/* Every commercial bot polls this on a ~15 minute cadence: it is the     */
/* most common P1 claimable (resources / speed-ups / occasional gems)    */
/* and the bot had no support for it at all. The response carries the    */
/* next open time, so the tick is driven by the server's own countdown   */
/* rather than a blind interval.                                         */
/* ------------------------------------------------------------------ */

static time_t g_gift_last = 0;

static void OnlineGiftTick(Connection *c)
{
	if (!c->wave.online_gift)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* Respect the server's countdown when we know it. */
	if (c->gift_next_open > c->server_time)
		return;

	/* Back off after a "not ready" so a stale or wrong countdown cannot
	 * turn this into a request every 10 seconds. */
	if (g_gift_last && (now - g_gift_last) < 300)
		return;
	g_gift_last = now;

	RequestOnlineGift(c);
	LOGI("[GIFT] free box requested (1117)\n");
}

static void ArenaTick(Connection *c)
{
	if (!c->wave.arena_challenge)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* --- gem prize ----------------------------------------------------
	 * The Colosseum pays gems by rank every 3 hours and the claim is
	 * free; 5214 is a confirmed empty payload. The reference bot calls
	 * this "Collect Arena Gems" and it is the entire free-gem value of
	 * the feature — the challenges themselves pay almost nothing.
	 * Polled on the game's own 3h cadence. */
	if (g_arena_prize_last == 0 ||
	    (now - g_arena_prize_last) >= 3 * 3600) {
		g_arena_prize_last = now;
		SendRaw(c, _MSG_REQUEST_ARENA_GET_PRIZE, NULL, 0);
		LOGI("[ARENA] gem prize claim requested (5214, free, 3h)\n");
	}

	if (!g_arena.have_target) {
		if (g_arena_refresh_last &&
		    (now - g_arena_refresh_last) < 60)
			return;
		g_arena_refresh_last = now;
		uint8_t kind = 0;
		SendRaw(c, _MSG_REQUEST_ARENA_REFRESH_TARGET, &kind, 1);
		LOGI("[ARENA] no targets yet — refresh requested (5205 kind=0)\n");
		return;
	}

	if (g_arena_last && (now - g_arena_last) < 600)
		return;
	g_arena_last = now;

	/* Full game payload: u8 idx | u32 place | char13 name | 5×u16 heroes */
	uint8_t payload[1 + 4 + 13 + 10];
	uint16_t pos = 0;
	payload[pos++] = 0;                          /* target index 0 */
	write_u32(payload + pos, g_arena.t_place);   pos += 4;
	memcpy(payload + pos, g_arena.t_name, 13);   pos += 13;
	for (int i = 0; i < 5; i++) {
		/* Offense squad. The game keeps offense and defense as
		 * separate formations; reusing the defense roster for the
		 * attack was a real correctness bug (the defense array is
		 * what other players fight). Falls back to the roster we
		 * have if no explicit offense set is configured. */
		uint16_t hero = c->wave.arena_offense_hero[i];
		if (hero == 0)
			hero = g_arena.def_hero[i];
		write_u16(payload + pos, hero);
		pos += 2;
	}
	SendRaw(c, _MSG_REQUEST_ARENA_CHALLENGE, payload, pos);
	LOGI("[ARENA] challenging rank %u (28-byte game payload, offense "
	     "squad)\n", g_arena.t_place);
}

/* ------------------------------------------------------------------ */
/* Wave D: reward claiming                                             */
/* ------------------------------------------------------------------ */

#define REWARD_BIT_TREASURE_DAILY 0x01
#define REWARD_BIT_DAILY          0x02
#define REWARD_BIT_ACHIEVEMENT    0x04
#define REWARD_BIT_EXPEDITION     0x08
#define REWARD_BIT_BATTLEPASS     0x10
#define REWARD_BIT_STAGE          0x20
#define REWARD_BIT_TREASURE_MONTH 0x40

static time_t g_reward_last = 0;

void WaveClaimRewards(Connection *c)
{
	if (!c->wave.reward_claim)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_reward_last && (now - g_reward_last) < 3600)
		return;
	g_reward_last = now;

	uint8_t mask = c->wave.reward_mask ? c->wave.reward_mask
	                                    : (REWARD_BIT_TREASURE_DAILY |
	                                       REWARD_BIT_DAILY);

	struct { uint8_t bit; uint16_t op; const char *name; } claims[] = {
		{ REWARD_BIT_TREASURE_DAILY, _MSG_REQUEST_GET_TREASUREDAILYGIFT, "treasure daily gift" },
		{ REWARD_BIT_DAILY,          _MSG_REQUEST_DAILY_PRIZE,          "daily mission prize" },
		{ REWARD_BIT_ACHIEVEMENT,    _MSG_REQUEST_ACHIEVEMENT_PRIZE,    "achievement prize" },
		{ REWARD_BIT_EXPEDITION,     _MSG_REQUEST_EXPEDITION_PRIZE,     "expedition prize" },
		{ REWARD_BIT_BATTLEPASS,     _MSG_REQUEST_BATTLEPASS_PRIZE,     "battle pass prize" },
		{ REWARD_BIT_STAGE,          _MSG_REQUEST_STAGE_PRIZE,          "stage prize" },
		{ REWARD_BIT_TREASURE_MONTH, _MSG_REQUEST_TREASURE_GET_MONTHPRIZE, "treasure month prize" },
	};

	int sent = 0;
	for (size_t i = 0; i < sizeof(claims) / sizeof(claims[0]); i++) {
		if (!(mask & claims[i].bit))
			continue;
		/* BETA payload: empty — most GET/collect packets carry no
		 * arguments; a wrong guess only yields a server error. */
		SendRaw(c, claims[i].op, NULL, 0);
		sent++;
		LOGI("[REWARD] claim sent: %s (opcode %u) — BETA payload\n",
		     claims[i].name, claims[i].op);
	}
	LOGI("[REWARD] %d claim packet(s) sent (mask=0x%02X)\n", sent, mask);
}

static void RewardTick(Connection *c)
{
	WaveClaimRewards(c);
}

/* ------------------------------------------------------------------ */
/* Wave D: traps                                                        */
/*                                                                      */
/* Layout confirmed against the decompiled client:                     */
/*   TRAPINFO (2602) resp: 12 x u32 quantity = 3 trap types x 4 tiers */
/*     (DataManager.cs: RecvTrapInfo). Index = type*4 + tier.          */
/*   TRAPCONSTEVENT (2604) resp: u8 kind | u8 rank | u32 qty |        */
/*     i64 begin | u32 need — the trap manufacturing queue.             */
/*   CONSTRUCT req (2605): u8 kind | u8 rank | u32 count               */
/*   REPAIR    req (2616): u8 kind | u8 rank | u32 count               */
/*                                                                      */
/* Traps stop working entirely once the wall hits 0 HP, and the wall   */
/* raises the per-type cap with its level, so the target is always     */
/* "fill the wall's cap".                                              */
/* ------------------------------------------------------------------ */

static time_t g_trap_last = 0;

/* 3 types x 4 tiers, from TRAPINFO. */
static uint32_t g_trap_qty[12];
static bool     g_trap_loaded = false;

/* Wall trap capacity by wall level (index 1..25). Derived from the
 * wiki's progression (100 at L1 growing to 125,000 at L25); the bot
 * only needs it to decide whether more traps are worth queuing, so an
 * approximation that is deliberately conservative is fine. */
static uint32_t WallTrapCapacity(uint8_t wall_level)
{
	if (wall_level == 0)
		return 0;
	/* Roughly geometric, anchored on the published endpoints. */
	uint64_t cap = 100;
	for (uint8_t i = 1; i < wall_level; i++)
		cap = cap * 135 / 100;
	return (uint32_t)(cap > 125000u ? 125000u : cap);
}

void WaveRecvTrapInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	/* 12 x u32 confirmed by the decompiled RecvTrapInfo. */
	if (size < 48) {
		LOGI("[TRAP] info too short (%uB, want >=48)\n", size);
		return;
	}
	uint16_t offset = 0;
	uint32_t total = 0;
	for (int i = 0; i < 12; i++) {
		g_trap_qty[i] = read_u32(data + offset); offset += 4;
		total += g_trap_qty[i];
	}
	if (!g_trap_loaded) {
		g_trap_loaded = true;
		LOGI("[TRAP] inventory: T%d %u/%u/%u/%u  T%d %u/%u/%u/%u  "
		     "T%d %u/%u/%u/%u  (total %u)\n",
		     1, g_trap_qty[0], g_trap_qty[1], g_trap_qty[2], g_trap_qty[3],
		     2, g_trap_qty[4], g_trap_qty[5], g_trap_qty[6], g_trap_qty[7],
		     3, g_trap_qty[8], g_trap_qty[9], g_trap_qty[10], g_trap_qty[11],
		     total);
	}
}

/* Trap repair.
 *
 * CONFIRMED against the decompiled client (DataManager.RecvTrapRepairInfo,
 * opcode 2604): 12 x u32 damaged-trap counts (index = type*4 + tier) +
 * 12 x u32 in-repair counts + i64 begin + u32 need. The live packet is
 * 112 bytes including its 4-byte header, which matches exactly.
 *
 * Damaged traps are permanently lost capacity: the wall's trap count
 * drops until they are repaired, so a wall that is never repaired is a
 * wall that slowly stops defending anything.
 */
static uint32_t g_trap_damaged[12];
static bool     g_trap_repair_loaded = false;

void WaveRecvTrapRepairInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	/* 12 hospital + 12 repair = 96B, then i64 + u32 = 12B. */
	if (size < 96) {
		LOGI("[TRAP] repair info short (%uB, want >=96)\n", size);
		return;
	}
	/* One dump: the layout is confirmed against the decompiled client
	 * but we have never seen a non-zero damaged set on this account, so
	 * the first non-empty capture is the proof the repair path needs. */
	static bool repair_dumped = false;
	if (!repair_dumped) {
		repair_dumped = true;
		WaveRecvDump(c, "TRAPREPAIRINFO", data, size);
	}

	uint16_t offset = 0;
	uint32_t damaged_total = 0, queued = 0;
	for (int i = 0; i < 12; i++) {
		g_trap_damaged[i] = read_u32(data + offset); offset += 4;
		damaged_total += g_trap_damaged[i];
	}
	for (int i = 0; i < 12; i++) {
		queued += read_u32(data + offset); offset += 4;
	}
	g_trap_repair_loaded = true;

	static bool logged = false;
	if (!logged && damaged_total) {
		logged = true;
		LOGI("[TRAP] damaged: %u total (T1 %u/%u/%u/%u  T2 %u/%u/%u/%u  "
		     "T3 %u/%u/%u/%u), %u already in repair\n",
		     damaged_total,
		     g_trap_damaged[0], g_trap_damaged[1],
		     g_trap_damaged[2], g_trap_damaged[3],
		     g_trap_damaged[4], g_trap_damaged[5],
		     g_trap_damaged[6], g_trap_damaged[7],
		     g_trap_damaged[8], g_trap_damaged[9],
		     g_trap_damaged[10], g_trap_damaged[11], queued);
	}
}

/* Traps are useless at 0 wall HP and are wasted if the wall is already
 * at capacity, so both are checked before queueing more. */
static void TrapTick(Connection *c)
{
	if (!c->wave.trap_build)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_trap_last && (now - g_trap_last) < 600)
		return;
	g_trap_last = now;

	/* Repairs first: damaged traps are capacity that is already lost,
	 * so this outranks building new ones. */
	if (g_trap_repair_loaded && c->wave.trap_repair) {
		for (int i = 0; i < 12; i++) {
			if (g_trap_damaged[i] == 0)
				continue;
			uint8_t type = (uint8_t)(i / 4);
			uint8_t rank = (uint8_t)(i % 4);
			uint32_t n = g_trap_damaged[i];
			if (c->wave.trap_repair_batch && n > c->wave.trap_repair_batch)
				n = c->wave.trap_repair_batch;
			if (n == 0)
				n = 1;

			/* Same shape as construct: kind, rank, count. */
			uint8_t payload[6];
			payload[0] = type;
			payload[1] = rank;
			write_u32(payload + 2, n);
			SendRaw(c, _MSG_REQUEST_REPAIRTRAP, payload, 6);
			g_trap_damaged[i] = 0;
			LOGI("[TRAP] repair type=%u tier=%u count=%u "
			     "(resp 2617)\n", type, rank + 1, n);
			return;
		}
	}

	if (!g_trap_loaded) {
		/* 2602 TRAPINFO is pushed by the server at login (verified:
		 * 48 bytes = 12 x u32, matching the decompiled RecvTrapInfo),
		 * so there is no request to make. Nothing to do until it
		 * arrives. */
		LOGI("[TRAP] waiting for TRAPINFO (2602) from the server\n");
		return;
	}

	/* Find the wall to know the cap. */
	uint8_t wall_level = 0;
	for (uint16_t i = 0; i < c->building_count; i++) {
		if (c->building[i].build_id == BUILD_WALL) {
			wall_level = c->building[i].level;
			break;
		}
	}
	if (wall_level == 0) {
		LOGI("[TRAP] no wall found among %u buildings, skipping\n",
		     c->building_count);
		return;
	}

	uint32_t built = 0;
	for (int i = 0; i < 12; i++)
		built += g_trap_qty[i];
	uint32_t cap = WallTrapCapacity(wall_level);

	if (built >= cap) {
		LOGI("[TRAP] at capacity: %u/%u (wall lv%u) — nothing to do\n",
		     built, cap, wall_level);
		return;
	}

	/* Fill the cheapest tier of a type that is furthest below an even
	 * split. Equalising keeps the wall effective against all three
	 * troop types instead of stacking one. */
	uint32_t target = cap / 3u;
	uint8_t  best_type = 0, best_rank = 0;
	uint32_t best_deficit = 0;
	for (uint8_t type = 0; type < 3; type++) {
		uint32_t have = 0;
		for (uint8_t rank = 0; rank < 4; rank++)
			have += g_trap_qty[type * 4 + rank];
		uint32_t deficit = (have < target) ? (target - have) : 0;
		if (deficit > best_deficit) {
			/* prefer the lowest rank within the type */
			for (uint8_t rank = 0; rank < 4; rank++) {
				if (g_trap_qty[type * 4 + rank] <
				    g_trap_qty[type * 4 + best_rank]) {
					best_rank = rank;
				}
			}
			best_deficit = deficit;
			best_type = type;
		}
	}
	if (best_deficit == 0) {
		LOGI("[TRAP] all types near target (%u/%u), skipping\n",
		     built, cap);
		return;
	}

	/* Queue a sensible batch rather than one at a time. */
	uint32_t want = 1000;
	if (want > best_deficit)
		want = best_deficit;
	if (want == 0)
		return;

	/* Game payload (UIBarrack_Soldier.SendTrapConstruct):
	 *   u8 (RD_Kind - 4) | u8 (RD_Rank - 1) | u32 count */
	uint8_t payload[6];
	payload[0] = best_type;
	payload[1] = best_rank;
	write_u32(payload + 2, want);
	SendRaw(c, _MSG_REQUEST_TRAPCONSTRUCT, payload, 6);
	LOGI("[TRAP] construct type=%u tier=%u count=%u (have %u/cap %u, "
	     "wall lv%u, resp 2606)\n",
	     best_type, best_rank + 1, want, built, cap, wall_level);
}

void WaveRecvTrapConstruct(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("TRAPCONSTRUCT_RESP", data, size, 32);
	if (size >= 1)
		LOGI("[TRAP] construct result=%u (0=ok)\n", data[0]);
	(void)c;
}

/* ------------------------------------------------------------------ */
/* Wave D: pets (learning mode)                                        */
/* ------------------------------------------------------------------ */

static time_t g_pet_last = 0;

void WaveRecvPetList(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview("PET_LIST", data, size, 96);
}

static void PetTick(Connection *c)
{
	if (!c->wave.pet_train)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_pet_last && (now - g_pet_last) < 600)
		return;
	g_pet_last = now;

	/* PET_LIST (8210) is a server push — training begin (8204) is
	 * wired once pet ids are parsed. */
	LOGI("[PET] auto-train armed — waiting for pet list push "
	     "(learning mode)\n");
}

/* ------------------------------------------------------------------ */
/* Wave F: hospital auto-heal (HEALINGTROOP 2426 -> RESP 2427)         */
/*                                                                      */
/* HOSPITALINFO (2425) arrives silently at login and fills             */
/* c->wounded (block 1 = hospital slot counts, block 2 = in-treatment, */
/* + num/total_time).  While wounded exist and no heal is running,    */
/* send a heal request; payload style is selectable (RequestHealTroops)*/
/* because the C# sender is not in our references — 2427 responses    */
/* are dumped raw so err==0 (accepted) validates the style.           */
/* ------------------------------------------------------------------ */

static time_t g_heal_last = 0;

void WaveRecvHealingTroop(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size == 0) {
		LOGI("[HEAL] empty response\n");
		return;
	}
	uint8_t err = data[0];
	if (err == 0) {
		LOGI("[HEAL] accepted — err=0 len=%u\n", size);
		HexPreview("HEAL_OK", data, size, 96);
	} else {
		LOGI("[HEAL] rejected — err=%u len=%u\n", err, size);
		HexPreview("HEAL_REJ", data, size, 96);
	}
}

void WaveRecvHealingComplete(Connection *c, const uint8_t *data,
                             uint16_t size)
{
	(void)c;
	/* Heal finished (2428) — HealTick re-arms on its own once the
	 * in-treatment block clears; just log the push. */
	HexPreview("HEAL_DONE", data, size, 64);
}

static void HealTick(Connection *c)
{
	if (!c->wave.heal_troops)
		return;
	if (c->server_time == 0)
		return;
	if (!c->wounded.loaded)
		return;
	if (c->wounded.troop.total == 0)
		return;
	/* Already healing? */
	if (c->wounded.healing.total > 0 || c->wounded.total_time > 0)
		return;

	time_t now = time(NULL);
	if (g_heal_last && (now - g_heal_last) < 180)
		return;
	g_heal_last = now;

	RequestHealTroops(c, c->wave.heal_style);
	LOGI("[HEAL] healing %u (slots) — style=%u tiers=%u "
	     "(watch 2427 err byte)\n",
	     c->wounded.troop.total, c->wave.heal_style,
	     c->wounded.troop.tiers);
}

/* ------------------------------------------------------------------ */
/* Wave F: hero roster (HEROSAVE 1201)                                 */
/*                                                                      */
/* C# RecvHeroSave: i64 unk | i16 count | count x { u16 id, u8 level,  */
/* u32 exp, u8 enhance, u8 star, u8 equip, 6xu8 enchant, 4xu8 skill }  */
/* (20 B/record in 1.80; later clients grew the record — we derive     */
/* the stride from the payload so both layouts parse).                 */
/* ------------------------------------------------------------------ */

void WaveRecvHeroSave(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size < 10) {
		HexPreview("HEROSAVE(short)", data, size, 64);
		return;
	}
	int64_t count = read_i16(data + 8);
	if (count <= 0 || size < 10) {
		HexPreview("HEROSAVE", data, size, 96);
		return;
	}
	uint16_t body = (uint16_t)(size - 10);
	if (body % (uint16_t)count != 0) {
		/* layout mismatch — keep raw for offline decode */
		HexPreview("HEROSAVE(unparsed)", data, size, 320);
		return;
	}
	uint16_t stride = body / (uint16_t)count;
	if (stride < 20) {
		HexPreview("HEROSAVE(short rec)", data, size, 320);
		return;
	}

	c->hero_count = 0;
	for (int64_t i = 0; i < count && c->hero_count < WAVE_HERO_MAX; i++) {
		uint16_t id = read_u16(data + 10 + i * stride);
		if (id != 0)
			c->hero_id[c->hero_count++] = id;
	}
	LOGI("[HERO] roster parsed: %u heroes (count=%ld stride=%u)\n",
	     c->hero_count, (long)count, stride);
}

/* ------------------------------------------------------------------ */
/* Training accounting: TRAINING_ (2408) accepted queue + ADDSOLDIER   */
/* (2409) soldiers delivered.  Live shapes (2026-09-28):               */
/*   2408 accepted: u8 err=0 | u8 kind | u8 tier | u32 qty |           */
/*                  5xu32 resources | i64 start | u32 total  (39/43B)  */
/*   2408 busy/rej: u8 err!=0 (1B or 5B)                               */
/*   2409:          u8 err=0 | u8 kind | u32 qty | 3xu32  (18B) —      */
/*                  tier absent; taken from the pending queue          */
/*   2402 info:     u8 err=0 | u8 kind | u32 qty | i64 start |         */
/*                  u32 total (18B) — seeds the pending queue at login */
/* ------------------------------------------------------------------ */

static void CreditSoldiers(Connection *c, uint8_t kind, uint8_t rank,
                           uint32_t qty, const char *why);

void WaveRecvTrainingResp(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("TRAINING_RESP", data, size, 64);
	if (size == 0)
		return;
	if (data[0] != 0) {
		LOGI("[TRAIN] rejected (err=%u) size=%u\n", data[0], size);
		return;
	}
	if (size >= 7) {
		c->train.pending_kind = data[1];
		c->train.pending_tier = data[2];
		c->train.pending_qty  = read_u32(data + 3);
		/* Wire layout (39B): ... i64 begin @27, u32 need @35. */
		c->train.pending_need = size >= 39 ? read_u32(data + 35) : 0;
		c->train.pending_since = (int64_t)time(NULL);
		LOGI("[TRAIN] queue accepted: %u x kind=%u tier=%u need=%us "
		     "(awaiting ADDSOLDIER)\n",
		     c->train.pending_qty, c->train.pending_kind,
		     c->train.pending_tier, c->train.pending_need);
		/* FREE-ITEMS RULE: 2407 FINISHTRAINING is a *diamond purchase*
		 * (its response literally carries `u32 diamonds`) — it is not
		 * a speed-up item and it violates the no-premium-spend policy.
		 * Removed; the batch now completes on its own timer. If a rush
		 * is ever wanted, it must go through earned Speed-Up-Training
		 * items via RequestSimpleUseItem, never 2407. */
		if (c->train.instant_finish) {
			LOGW("[TRAIN] train.instant_finish is ignored: 2407 "
			     "spends diamonds (free-items rule)\n");
		}
	}
}

void WaveRecvTrainingInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("TRAININGINFO_RESP", data, size, 32);
	if (size < 6 || data[0] != 0)
		return;
	uint32_t qty = read_u32(data + 2);
	/* Mid-queue reconnect: seed pending from the active batch (tier
	 * is not in this push — default T1, correct for our config). */
	if (c->train.pending_qty == 0 && qty > 0 && qty <= 100000) {
		c->train.pending_kind = data[1];
		c->train.pending_tier = 0;
		c->train.pending_qty  = qty;
		c->train.pending_need = 0;
		c->train.pending_since = (int64_t)time(NULL);
		LOGI("[TRAIN] active queue from info: %u x kind=%u "
		     "(awaiting ADDSOLDIER)\n", qty, data[1]);
	}
}

void WaveRecvAddSoldier(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("ADDSOLDIER", data, size, 48);
	if (size < 6) {
		LOGI("[TRAIN] ADDSOLDIER short size=%u\n", size);
		return;
	}
	/* Official RecvAddSoldier: NO err byte — kind, rank, qty. */
	uint8_t kind = data[0];
	uint8_t rank = data[1];
	uint32_t qty = read_u32(data + 2);
	if (c->train.pending_qty && qty == c->train.pending_qty) {
		c->train.pending_qty = 0;
		CreditSoldiers(c, kind, rank, qty, "delivered");
	} else {
		/* No pending match — credit the total so the gather gate
		 * sees the army; next ARMYYROUP/TROOPHOME push resyncs. */
		CreditSoldiers(c, kind, rank, qty, "delivered(untracked)");
	}
}

/* ------------------------------------------------------------------ */
/* Alliance probes (2809/2857 discovery, live-run 2026-09-29).        */
/*   2810 APPLY resp: 33B — first byte = result code (0 = ok/queued). */
/*   2858 PUBLICINFO resp: err | id | ... name/tag/approval/member;   */
/*      the 1300-byte notice dominates the body, so dump head+tail.   */
/*   2818 SEARCH resp (5B) + 2820 SEARCHRESULT (list).                */
/* ------------------------------------------------------------------ */
void WaveRecvAllianceApply(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("ALLIANCE_APPLY_RESP", data, size, 96);
	if (size)
		LOGI("[ALLY] apply result first-byte=%u size=%u\n",
		     data[0], size);
}

void WaveRecvAlliancePublicInfo(Connection *c, const uint8_t *data,
                               uint16_t size)
{
	HexPreview("ALLIANCE_PUBLICINFO_HEAD", data, size, 96);
	if (size > 96)
		HexPreview("ALLIANCE_PUBLICINFO_TAIL", data + size - 64,
		           64, 64);
}

void WaveRecvAllianceSearch(Connection *c, const uint8_t *data,
                           uint16_t size)
{
	HexPreview("ALLIANCE_SEARCH_RESP", data, size, 64);
}

void WaveRecvAllianceSearchResult(Connection *c, const uint8_t *data,
                                  uint16_t size)
{
	HexPreview("ALLIANCE_SEARCHRESULT", data, size, 640);
}

/* TROOPMARCH_NOTATK resp (6616) — semantics learned live. */
void WaveRecvMarchNotAtk(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("MARCH_NOTATK_RESP", data, size, 64);
	if (size == 0)
		return;
	if (data[0] != 0) {
		LOGI("[GATHER] march rejected (err=%u) size=%u\n",
		     data[0], size);
		if (c->player.current_marches > 0)
			c->player.current_marches--;
	} else {
		LOGI("[GATHER] march accepted (size=%u, marches=%u/%u)\n",
		     size, c->player.current_marches, c->player.max_marches);
	}
}

/* ------------------------------------------------------------------ */
/* March lifecycle (official DataManager.cs parses, live-verified):    */
/*   2407 FINISH resp: err | u32 diamonds | u8 kind | u8 rank |        */
/*        u32 qty | u32 foodStock | i64 (23B) — delivers soldiers here */
/*   2409 ADDSOLDIER: u8 kind | u8 rank | u32 qty | u32 food | i64     */
/*        (18B, NO err byte — kind 0..3)                               */
/*   2416 TROOPMARCH resp: err(0=ok slot/type/heroes/troops/dest...)   */
/*        or err 1..8 reject codes (1 = march limit)                   */
/*   2418 TROOPRETURN: slot u8 | type u8 | begin i64 | need u32        */
/*   2419 TROOPHOME: slot u8 | 16..20xu32 home army | 5xu32 stocks     */
/*   2421 GATHERINGEVENT: slot u8 | begin i64 | need u32 | overload u32 */
/* ------------------------------------------------------------------ */

static void CreditSoldiers(Connection *c, uint8_t kind, uint8_t rank,
                           uint32_t qty, const char *why)
{
	if (kind < 4 && rank < TROOP_MAX_TIERS) {
		/* kinds[] is the kind-major view used by the training
		 * scheduler; keep it as the single source of truth so the
		 * per-kind arrays and the rotation maths can never drift. */
		c->troop.kinds[kind][rank] += qty;
		uint32_t *arr[4] = { c->troop.infantry, c->troop.ranged,
		                     c->troop.cavalry, c->troop.siege };
		arr[kind][rank] = c->troop.kinds[kind][rank];
	}
	if (kind < 4 && c->troop.tiers < rank + 1)
		c->troop.tiers = rank + 1;
	c->troop.total += qty;
	LOGI("[TRAIN] %s +%u T%u kind=%u -> total=%u\n",
	     why, qty, rank + 1, kind, c->troop.total);
}

/* 2405 dismiss result. The local ledger is already decremented when the
 * request is sent, so this only reports what the server actually did —
 * if it rejects, the counts must be re-synced from the next army push
 * (2401) rather than left wrong. */
void WaveRecvTroopDismiss(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("TROOPDISMISS_RESP", data, size, 24);
	if (size == 0)
		return;
	if (data[0] != 0) {
		LOGI("[TRAIN] dismiss rejected (err=%u) — local counts will "
		     "re-sync on the next army push\n", data[0]);
		return;
	}
	LOGI("[TRAIN] dismiss accepted (size=%u)\n", size);
}

void WaveRecvFinishTraining(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("FINISH_TRAINING", data, size, 48);
	if (size == 0)
		return;
	if (data[0] != 0) {
		LOGI("[TRAIN] finish rejected (err=%u) size=%u\n",
		     data[0], size);
		return;
	}
	if (size < 11) {
		LOGI("[TRAIN] finish short size=%u\n", size);
		return;
	}
	uint8_t kind = data[5], rank = data[6];
	uint32_t qty = read_u32(data + 7);
	CreditSoldiers(c, kind, rank, qty, "finished");
	if (c->train.pending_qty == qty)
		c->train.pending_qty = 0;
}

/* Active gather marches (slot from the 6615 accept resp), so an
 * incoming attack can recall them (TROOPRETURN 2417). */
struct GatherSlot {
	uint8_t  active;
	uint8_t  recalled;      /* take-back already sent for this slot. */
	uint16_t zone;
	uint8_t  point;
};
static struct GatherSlot g_gm[8];
static uint8_t  g_gm_pending;            /* gather march awaiting accept */
static uint16_t g_gm_pending_zone;
static uint8_t  g_gm_pending_point;

static void GatherSlotClear(uint8_t slot)
{
	if (slot < 8 && g_gm[slot].active) {
		LOGI("[GATHER] slot=%u zone=%u pt=0x%02x done\n",
		     slot, g_gm[slot].zone, g_gm[slot].point);
		g_gm[slot].active = 0;
		g_gm[slot].recalled = 0;
	}
}

/* TROOPRETURN every tracked gather march (defense / damage control). */
void GatherRecallAll(Connection *c, const char *why)
{
	if (!c->protection.recall_on_incoming_attack)
		return;

	/* A threat just landed on a tile we were working. Pause dispatching
	 * for a moment so we do not walk straight back into it. */
	GatherSetRegatherCooldown(c->wave.regather_cooldown_s);

	int n = 0;
	for (uint8_t s = 0; s < 8; s++) {
		if (!g_gm[s].active || g_gm[s].recalled)
			continue;
		RequestTroopTakeBack(c, s);
		g_gm[s].recalled = 1;
		n++;
		LOGI("[RECALL] slot=%u zone=%u pt=0x%02x why=%s\n",
		     s, g_gm[s].zone, g_gm[s].point, why);
	}
	if (!n)
		LOGI("[RECALL] %s: no active gather march\n", why);
}

/* 2435 BEINGATTACK — server resynced army/hospital after a hit on us. */
void WaveRecvBeingAttacked(Connection *c, const uint8_t *data,
                           uint16_t size)
{
	LOGI("[ATK] BEINGATTACK size=%u (battle resync)\n", size);
	GatherRecallAll(c, "being-attacked");
	(void)data;
}

void WaveRecvTroopMarch(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("TROOPMARCH_RESP", data, size, 128);
	if (size == 0)
		return;
	if (data[0] == 0) {
		/* Track the slot when this accept answers our gather 6615,
		 * so an incoming attack can TROOPRETURN (2417) it. */
		if (g_gm_pending && size >= 2) {
			uint8_t slot = data[1];
			uint16_t zone = g_gm_pending_zone;
			uint8_t point = g_gm_pending_point;
			if (size >= 80) {          /* dest echoed on the wire */
				zone = read_u16(data + 77);
				point = data[79];
			}
			g_gm_pending = 0;
			if (slot < 8) {
				g_gm[slot].active = 1;
				g_gm[slot].recalled = 0;
				g_gm[slot].zone = zone;
				g_gm[slot].point = point;
				LOGI("[GATHER] tracking slot=%u zone=%u "
				     "pt=0x%02x\n", slot, zone, point);
			}
		}
		/* Official RecvTroopMarch: err | slot | type | 5xu16 heroes
		 * | 16xu32 troops (kind-major x4 tiers) | dest ... — the
		 * marching troops leave the home army (TROOPHOME resyncs). */
		if (size >= 77) {
			uint32_t *arr[4] = { c->troop.infantry, c->troop.ranged,
			                     c->troop.cavalry, c->troop.siege };
			uint32_t out = 0;
			for (int k = 0; k < 4; k++)
				for (int t = 0; t < 4; t++) {
					uint32_t v = read_u32(data + 13 +
					                       (k * 4 + t) * 4);
					out += v;
					if (arr[k][t] >= v)
						arr[k][t] -= v;
					else
						arr[k][t] = 0;
				}
			if (c->troop.total >= out)
				c->troop.total -= out;
			else
				c->troop.total = 0;
			LOGI("[MARCH] accepted slot=%u troops_out=%u -> "
			     "home=%u (marches=%u/%u)\n",
			     data[1], out, c->troop.total,
			     c->player.current_marches, c->player.max_marches);
		} else {
			LOGI("[MARCH] accepted slot=%u (marches=%u/%u)\n",
			     data[1], c->player.current_marches,
			     c->player.max_marches);
		}
	} else {
		static const char *why[] = { "", "march limit", "busy",
			"invalid target", "err4", "err5", "err6", "err7",
			"yolk zone" };
		const char *w = data[0] < 9 ? why[data[0]] : "?";
		LOGI("[MARCH] rejected err=%u (%s)\n", data[0], w);
		g_gm_pending = 0;
		if (c->player.current_marches > 0)
			c->player.current_marches--;
	}
}

void WaveRecvTroopReturn(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size >= 14) {
		uint8_t slot = data[0], type = data[1];
		int64_t begin = read_i64(data + 2);
		uint32_t need = read_u32(data + 10);
		LOGI("[MARCH] slot=%u returning type=%u begin=%ld eta=%us\n",
		     slot, type, (long)begin, need);
		GatherSlotClear(slot);
	} else {
		HexPreview("TROOPRETURN", data, size, 48);
	}
}

/* slot u8 | 20xu32 home army (kind-major x5 tiers) | 5xu32 stocks = 101B.
 * Authoritative resync of troops + resource stocks (incl. gathered). */
void WaveRecvTroopHome(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size < 101) {
		HexPreview("TROOPHOME", data, size, 160);
		if (c->player.current_marches > 0)
			c->player.current_marches--;
		return;
	}
	uint8_t slot = data[0];
	GatherSlotClear(slot);
	uint32_t *arr[4] = { c->troop.infantry, c->troop.ranged,
	                     c->troop.cavalry, c->troop.siege };
	uint32_t total = 0;
	for (int k = 0; k < 4; k++)
		for (int t = 0; t < 5; t++) {
			arr[k][t] = read_u32(data + 1 + (k * 5 + t) * 4);
			total += arr[k][t];
		}
	c->troop.total = total;
	c->troop.tiers = 5;
	c->troop.loaded = true;

	const uint8_t *st = data + 81;
	c->resources.food = read_u32(st);
	c->resources.rock = read_u32(st + 4);
	c->resources.wood = read_u32(st + 8);
	c->resources.ore  = read_u32(st + 12);
	c->resources.gold = read_u32(st + 16);
	c->resources_last_update = c->server_time;

	if (c->player.current_marches > 0)
		c->player.current_marches--;
	LOGI("[MARCH] slot=%u home army=%u food=%u rock=%u wood=%u "
	     "ore=%u gold=%u (marches=%u/%u)\n",
	     slot, total, c->resources.food, c->resources.rock,
	     c->resources.wood, c->resources.ore, c->resources.gold,
	     c->player.current_marches, c->player.max_marches);
}

void WaveRecvGatheringEvent(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("GATHERINGEVENT", data, size, 48);
	if (size < 13) {
		LOGI("[GATHER] event short size=%u\n", size);
		return;
	}
	uint8_t slot = data[0];
	if (slot >= 8) {
		LOGI("[GATHER] event bad slot=%u\n", slot);
		return;
	}
	int64_t begin = read_i64(data + 1);
	uint32_t need = read_u32(data + 9);
	LOGI("[GATHER] active slot=%u begin=%ld eta=%us\n",
	     slot, (long)begin, need);
}

/* u32 id | u8 | i64 time | u16 kingdom | u16 zone | u8 point |
 * u8 kind | u8 level | u32 amount | 5B  (29B observed) */
void WaveRecvGatherReport(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size >= 24) {
		uint16_t zone = read_u16(data + 15);
		uint8_t point = data[17];
		uint8_t kind = data[18];
		uint32_t amount = read_u32(data + 20);
		LOGI("[GATHER] report zone=%u pt=0x%02x kind=%u amount=%u "
		     "(size=%u)\n", zone, point, kind, amount, size);
		/* Tile fully gathered — clear in-flight marker so the next
		 * scan can re-evaluate it. */
		for (uint16_t i = 0; i < c->map_tile_count; i++) {
			MapTile *t = &c->map_tiles[i];
			if (t->zone == zone && t->point == point)
				t->amount = 0;
		}
	} else {
		HexPreview("GATHERREPORT", data, size, 64);
		LOGI("[GATHER] report size=%u\n", size);
	}
}

/* ------------------------------------------------------------------ */
/* Wave C: map scanning (gather / monster hunt)                        */
/* ------------------------------------------------------------------ */

static time_t g_scan_last = 0;
/* zone-0 overview fallback state (see MapScanTick) */
static time_t     g_scan_overview_last = 0;
static uint32_t   g_scan_overview_tries = 0;
static uint32_t   g_scan_updates_at_last_overview = 0;
static time_t     g_scan_nowarn = 0;
static uint16_t   g_scan_zone = 0;

/* UPDATE_MAPINFO wire (validated live: castle tile = our own
 * K1447 X317 Y45 record):
 *   15B header: u8 type | u16 A | u16 zone_hint | u16 B |
 *               3xu16 0 | u16 0x3c33
 *   N x 51B records: u16 zone | u8 point | u8 kind(POINT_KIND) |
 *     kind 1..7 resource: pad.. | u8 level@body[18] | u32 amount@body[19]
 *     kind 8/9 city/camp:  char name[16] | u16 kingdom | u8 level |
 *     kind 10 npc/monster: u8 01 | u8 level | u8 00 | u16 id | ...
 * POINT_KIND: 1 food 2 stone 3 ore 4 wood 5 gold 6 crystal 7 sp,
 *             8 city 9 camp 10 npc (monster/darknest-ish). */
void WaveRecvMapUpdate(Connection *c, uint16_t opcode,
                       const uint8_t *data, uint16_t size)
{
	/* 15B header + N x 51B records, sometimes a trailing status
	 * block (60B seen → remainder 9).  Bigger remainders are a
	 * different packet shape — keep them raw. */
	if (size < 15 || ((size - 15) % 51) > 16) {
		LOGI("[MAP] size=%u op=%u not 15+51N — raw dump\n", size, opcode);
		HexPreview("MAP_UPDATE", data, size, 640);
		return;
	}

	uint16_t n = (size - 15) / 51;
	uint16_t res = 0, npc = 0, city = 0, fresh = 0;
	uint16_t trailer = 0;
	const uint8_t *r = data + 15;

	for (uint16_t i = 0; i < n; i++, r += 51) {
		MapTile t;
		t.zone   = read_u16(r);
		t.point  = r[2];
		t.kind   = r[3];
		t.level  = 0;
		t.amount = 0;

		/* Zone-0 overview packets (pre-role scans) and the 60B
		 * trailing status block land here — stop, don't store. */
		if (t.zone == 0 || t.zone > 4096) {
			trailer = (uint16_t)(size - (uint16_t)(r - data));
			break;
		}

		const uint8_t *body = r + 4;
		if (t.kind >= 1 && t.kind <= 7) {
			t.level  = body[18];
			t.amount = read_u32(body + 19);
			res++;
		} else if (t.kind == 8 || t.kind == 9) {
			city++;
		} else if (t.kind == 10) {
			t.level = body[1];
			npc++;
		}

		/* upsert by (zone, point) */
		MapTile *slot = NULL;
		for (uint16_t j = 0; j < c->map_tile_count; j++) {
			if (c->map_tiles[j].zone == t.zone &&
			    c->map_tiles[j].point == t.point) {
				slot = &c->map_tiles[j];
				break;
			}
		}
		if (!slot) {
			if (c->map_tile_count >= MAP_TILE_MAX)
				continue;
			slot = &c->map_tiles[c->map_tile_count++];
			fresh++;
		}
		*slot = t;
	}

	c->map_tile_updates++;
	LOGI("[MAP] +%u tiles (res=%u npc=%u city=%u trailer=%u) fresh=%u "
	     "total=%u op=%u\n",
	     n, res, npc, city, trailer, fresh, c->map_tile_count, opcode);
}

/* tile distance in map cells (x from getTileMapPosbyMapID) */
static int TileDist(map_pos_t a, map_pos_t b)
{
	int dx = (int)a.x - (int)b.x;
	int dy = (int)a.y - (int)b.y;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	return dx > dy ? dx : dy;
}

static time_t g_gather_last = 0;
/* "Time to wait before regathering": after a tile is attacked or
 * scouted the bot must not immediately re-march into the same contested
 * tile — the attacker is usually still there, and re-marching
 * re-telegraphs the account to anyone watching the map. */
static time_t g_regather_block_until = 0;
static time_t g_gather_block_logged = 0;

void GatherSetRegatherCooldown(uint32_t seconds)
{
	if (seconds == 0)
		return;
	g_regather_block_until = time(NULL) + (time_t)seconds;
	LOGI("[GATHER] regathering paused for %us after a threat\n", seconds);
}
static time_t g_hunt_last = 0;
static time_t g_notroop_warn = 0;
static time_t g_gather_nowarn = 0;

/* Pick a resource tile and march (TROOPMARCH_NOTATK 6615).
 * Returns true when a march was sent (hunt defers to gather —
 * both march the same army). */
static bool GatherTick(Connection *c)
{
	if (!c->wave.gather_auto || c->server_time == 0)
		return false;

	time_t now = time(NULL);
	uint32_t interval = c->wave.gather_interval_s ? c->wave.gather_interval_s
	                                               : 60;
	if (g_gather_last && (now - g_gather_last) < (time_t)interval)
		return false;

	/* Do not re-march into a tile that was just attacked or scouted:
	 * the attacker is often still standing there, and re-marching
	 * re-telegraphs the account to everyone watching the map. */
	if (now < g_regather_block_until) {
		if (now > g_gather_block_logged) {
			g_gather_block_logged = now + 60;
			LOGI("[GATHER] regather cooldown, %llds left\n",
			     (long long)(g_regather_block_until - now));
		}
		return false;
	}

	if (c->troop.total == 0) {
		if (!g_notroop_warn || (now - g_notroop_warn) > 600) {
			g_notroop_warn = now;
			LOGI("[GATHER] waiting: no troops trained\n");
		}
		return false;
	}
	if (c->map_tile_count == 0)
		return false;
	if (c->player.max_marches &&
	    c->player.current_marches >= c->player.max_marches)
		return false;

	map_pos_t home = getTileMapPosbyPointCode(c->player.zone_id,
	                                           c->player.point_id);
	int best = -1;
	uint32_t best_amt = 0;
	int best_dist = 0;
	uint64_t best_score = 0;
	uint8_t best_kind = 0;

	for (uint16_t i = 0; i < c->map_tile_count; i++) {
		MapTile *t = &c->map_tiles[i];
		if (t->kind < 1 || t->kind > 7)
			continue;
		if (t->zone != c->player.zone_id)   /* same-zone only */
			continue;
		if (t->amount == 0)               /* empty or already marched */
			continue;
		/* Gem lodes are POINT_KIND 6 (PK_CRYSTAL). They are free
		 * currency, so when gather_gems_first is set they jump the
		 * queue regardless of level or distance. The resource
		 * min_amount filter deliberately does NOT apply to them: a
		 * 10-gem L1 lode is still a correct target. */
		bool is_gem = (t->kind == 6);
		if (!is_gem) {
			uint32_t min_amt = c->wave.gather_min_amount;
			if (min_amt && t->amount < min_amt)
				continue;
		} else if (c->wave.gather_gem_min_level &&
		           t->level < c->wave.gather_gem_min_level) {
			continue;
		}
		map_pos_t p = getTileMapPosbyMapID(
			PointCodeToMapID(t->zone, t->point));
		int d = TileDist(home, p);
		/* Gem lodes are worth a longer walk than ordinary resources,
		 * but not an unbounded one. */
		uint32_t max_d = c->wave.gather_max_dist;
		if (is_gem && c->wave.gather_gem_max_dist)
			max_d = c->wave.gather_gem_max_dist;
		if (max_d && d > (int)max_d)
			continue;

		if (is_gem && c->wave.gather_gems_first) {
			/* Strictly outranks every resource tile, best lode by
			 * gems-remaining then distance. */
			bool gbetter = (best < 0) || best_kind != 6 ||
			               (t->amount > best_amt) ||
			               (t->amount == best_amt && d < best_dist);
			if (gbetter) {
				best = i; best_amt = t->amount;
				best_dist = d; best_score = 0;
				best_kind = t->kind;
			}
			continue;
		}

		/* Tile priority: 0 = most amount, 1 = amount per travel
		 * cell (near + rich), 2 = nearest first. */
		bool better;
		uint64_t score = 0;
		switch (c->wave.gather_priority) {
		case 0:
			better = (best < 0) || t->amount > best_amt;
			break;
		case 2:
			better = (best < 0) || d < best_dist ||
			         (d == best_dist && t->amount > best_amt);
			break;
		case 3: {
			/* Lowest-stock: prioritise the resource the castle is
			 * shortest of, against a configurable target rather
			 * than a capacity field (the bot does not track vault
			 * capacity per resource). Distance still gates it, so
			 * this never walks across the map for a rounding error.
			 * Strictly better than amount-maximising for a growing
			 * account, which otherwise keeps piling into whichever
			 * resource it already has most of. */
			uint64_t have = 0;
			switch (t->kind) {
			case 1: have = c->resources.food; break;
			case 2: have = c->resources.rock; break;
			case 3: have = c->resources.ore;  break;
			case 4: have = c->resources.wood; break;
			case 5: have = c->resources.gold; break;
			default: have = 0;               break;
			}
			uint64_t target = c->wave.gather_stock_target
			                ? c->wave.gather_stock_target : 1000000;
			/* Shortfall dominates; amount and distance break ties. */
			score = (have < target) ? (target - have) : 1;
			score = score * 1000u / (uint64_t)(d + 1);
			score = score + (uint64_t)t->amount;
			better = (best < 0) || score > best_score;
			break;
		}
		default: /* 1 mixed */
			score = (uint64_t)t->amount * 1000u /
			        (uint64_t)(d + 1);
			better = (best < 0) || score > best_score;
			break;
		}
		if (better) {
			best = i;
			best_amt = t->amount;
			best_dist = d;
			best_score = score;
			best_kind = t->kind;
		}
	}
	if (best < 0) {
		/* Previously silent: the bot could sit for hours with a full
		 * army, free march slots and gather_auto=true and never say
		 * why. Report what the selector actually saw. */
		if (!g_gather_nowarn || (now - g_gather_nowarn) > 600) {
			g_gather_nowarn = now;
			uint16_t res_seen = 0, wrong_zone = 0, in_flight = 0;
			uint16_t under_min = 0, over_dist = 0;
			for (uint16_t i = 0; i < c->map_tile_count; i++) {
				MapTile *t = &c->map_tiles[i];
				if (t->kind < 1 || t->kind > 7)
					continue;
				res_seen++;
				if (t->zone != c->player.zone_id) {
					wrong_zone++;
					continue;
				}
				if (t->amount <= 1) {
					in_flight++;
					continue;
				}
				if (c->wave.gather_min_amount &&
				    t->amount < c->wave.gather_min_amount) {
					under_min++;
					continue;
				}
				int d = TileDist(home, getTileMapPosbyMapID(
					PointCodeToMapID(t->zone, t->point)));
				if (c->wave.gather_max_dist &&
				    d > (int)c->wave.gather_max_dist)
					over_dist++;
			}
			LOGI("[GATHER] no candidate tile: %u tiles known, "
			     "%u resource tiles (in-flight=%u wrong-zone=%u "
			     "under-min=%u over-dist=%u, min_amount=%u "
			     "max_dist=%u)\n",
			     c->map_tile_count, res_seen, in_flight, wrong_zone,
			     under_min, over_dist, c->wave.gather_min_amount,
			     c->wave.gather_max_dist);
		}
		return false;
	}

	MapTile *t = &c->map_tiles[best];
	g_gather_last = now;

	/* Fit the army to the tile: carry capacity = count x load, so a
	 * small nearby tile needs only a small march. */
	uint32_t troops16[16];
	const uint32_t *send16 = NULL;
	if (c->wave.gather_fit && t->amount) {
		uint32_t load = c->wave.gather_load ? c->wave.gather_load : 10;
		/* Gem lodes invert the usual ratio: 1000 army capacity carries
		 * exactly 1 gem, versus ~10 resources per troop on a normal
		 * tile. Using the resource divisor here would send a fraction
		 * of the needed force and trickle gems instead of clearing the
		 * lode before the next spawn. */
		if (t->kind == 6 && c->wave.gather_gem_load)
			load = c->wave.gather_gem_load;
		const uint32_t *kinds[4] = { c->troop.infantry, c->troop.ranged,
		                             c->troop.cavalry, c->troop.siege };
		uint64_t cap = 0;
		uint32_t have = 0;
		for (int k = 0; k < 4; k++)
			for (int ti = 0; ti < 4; ti++) {
				cap += (uint64_t)kinds[k][ti] * load;
				have += kinds[k][ti];
			}
		/* scale = min(1, need/cap) in 1/1024 units, floor 32 so the
		 * march is never a single scout; ceil each line. */
		uint32_t scale = 1024;
		if (cap && (uint64_t)t->amount < cap)
			scale = (uint32_t)(((uint64_t)t->amount * 1024u) / cap);
		if (scale < 32 && have)
			scale = 32;
		uint32_t send = 0;
		for (int k = 0; k < 4; k++)
			for (int ti = 0; ti < 4; ti++) {
				uint32_t v = kinds[k][ti];
				v = (uint32_t)(((uint64_t)v * scale + 1023u) /
				               1024u);
				if (v && !kinds[k][ti])
					v = 0;
				if (kinds[k][ti] && scale >= 32 && v == 0)
					v = 1;       /* keep the line alive */
				troops16[k * 4 + ti] = v;
				send += v;
			}
		if (t->kind == 6) {
			LOGI("[GATHER] gem lode level=%u gems=%u load=%u "
			     "cap=%llu send=%u/%u troops (1000 capacity per "
			     "gem)\n", t->level, t->amount, load,
			     (unsigned long long)cap, send, have);
		}
		if (send && send < have) {
			send16 = troops16;
			LOGI("[GATHER] fit: amount=%u cap=%llu load=%u "
			     "scale=%u/1024 send=%u/%u\n",
			     t->amount, (unsigned long long)cap, load,
			     scale, send, have);
		}
	}

	g_gm_pending = 1;
	g_gm_pending_zone = t->zone;
	g_gm_pending_point = t->point;
	RequestGatherMarch(c, t->zone, t->point, send16);
	LOGI("[GATHER] 6615 march -> zone=%u point=0x%02x kind=%u "
	     "amount=%u dist=%d prio=%u%s\n",
	     t->zone, t->point, t->kind, t->amount, best_dist,
	     c->wave.gather_priority, send16 ? " (fit)" : " (full)");
	t->amount = 0;   /* in-flight marker until tile refresh */
	return true;
}

/* Pick a monster tile and attack (SENDMONSTER 2488). */
static void HuntTick(Connection *c)
{
	if (!c->wave.hunt_auto || c->server_time == 0)
		return;

	time_t now = time(NULL);
	uint32_t interval = c->wave.gather_interval_s ? c->wave.gather_interval_s
	                                               : 60;
	if (g_hunt_last && (now - g_hunt_last) < (time_t)interval)
		return;

	if (c->troop.total == 0) {
		if (!g_notroop_warn || (now - g_notroop_warn) > 600) {
			g_notroop_warn = now;
			LOGI("[HUNT] waiting: no troops trained\n");
		}
		return;
	}
	if (c->map_tile_count == 0)
		return;
	if (c->player.max_marches &&
	    c->player.current_marches >= c->player.max_marches)
		return;

	map_pos_t home = getTileMapPosbyPointCode(c->player.zone_id,
	                                           c->player.point_id);
	int best = -1, best_level = -1, best_dist = 0;

	for (uint16_t i = 0; i < c->map_tile_count; i++) {
		MapTile *t = &c->map_tiles[i];
		if (t->kind != 10)
			continue;
		if (t->zone != c->player.zone_id)   /* same-zone only */
			continue;
		if (t->amount)                    /* already marched */
			continue;
		if (c->wave.hunt_min_level && t->level < c->wave.hunt_min_level)
			continue;
		if (c->wave.hunt_max_level && t->level > c->wave.hunt_max_level)
			continue;
		map_pos_t p = getTileMapPosbyMapID(
			PointCodeToMapID(t->zone, t->point));
		int d = TileDist(home, p);
		if (c->wave.gather_max_dist && d > (int)c->wave.gather_max_dist)
			continue;
		if (t->level > best_level) {
			best_level = t->level;
			best = i;
			best_dist = d;
		}
	}
	if (best < 0)
		return;

	MapTile *t = &c->map_tiles[best];
	g_hunt_last = now;
	RequestHuntMarch(c, t->zone, t->point, 1);
	LOGI("[HUNT] 2488 attack -> zone=%u point=0x%02x level=%u "
	     "dist=%d (BETA, watch SENDMONSTER_RESP)\n",
	     t->zone, t->point, t->level, best_dist);
	t->amount = 1;   /* in-flight marker until tile refresh */
}

static void MapScanTick(Connection *c)
{
	if (!c->wave.gather_auto && !c->wave.hunt_auto)
		return;
	if (c->server_time == 0)
		return;
	/* Don't burn the scan interval (or ask for a zone-0 overview)
	 * before role info gives us the real zone. */
	if (c->player.zone_id == 0 || c->player.name[0] == '\0') {
		if (!g_scan_nowarn || (time(NULL) - g_scan_nowarn) > 300) {
			g_scan_nowarn = time(NULL);
			LOGW("[SCAN] waiting for role info (zone=%u name='%s')\n",
			     c->player.zone_id, c->player.name);
		}
		return;
	}

	/* ROLEINFO can arrive tens of seconds into the session. Reset the
	 * interval gate on the transition so the first real scan goes out
	 * immediately instead of waiting out a stale timer. */
	if (c->player.zone_id != g_scan_zone) {
		g_scan_zone = c->player.zone_id;
		g_scan_last = 0;
	}

	time_t now = time(NULL);
	uint32_t interval = c->wave.scan_interval_s ? c->wave.scan_interval_s
	                                             : 300;
	if (g_scan_last && (now - g_scan_last) < (time_t)interval)
		return;
	g_scan_last = now;

	/* Zone-0 overview fallback.
	 *
	 * Evidence from earlier runs: when the FIRST map request of a
	 * session was the zone-0 overview the server answered with the
	 * whole kingdom window (run31: +39 tiles, res=25) and gathering
	 * worked. When we only ever ask for the player's own zone the
	 * server answers once with a tiny neighbourhood (runs 54/55:
	 * +8 tiles, res=0) and then goes silent for the rest of the
	 * session, so the selector never sees a kind 1..7 tile and gather
	 * never fires.
	 *
	 * So: keep asking for the home zone, but if the tile cache has not
	 * grown (no resource candidates) for a while, ask for the overview
	 * instead. Both requests are read-only. */
	if (c->map_tile_updates == g_scan_updates_at_last_overview) {
		if (g_scan_overview_last == 0 ||
		    (now - g_scan_overview_last) >= (time_t)(interval * 2)) {
			g_scan_overview_last = now;
			g_scan_overview_tries++;
			uint16_t overview[4] = { 0, 0, 0, 0 };
			RequestMapData(c, 1, overview);
			LOGI("[SCAN] zone %u has no new map data — requesting "
			     "kingdom overview (try %u, %u tiles known)\n",
			     c->player.zone_id, g_scan_overview_tries,
			     c->map_tile_count);
			return;
		}
	}
	g_scan_updates_at_last_overview = c->map_tile_updates;

	uint16_t zones[4] = { c->player.zone_id, 0, 0, 0 };
	RequestMapData(c, 1, zones);
	LOGI("[SCAN] map data requested for zone %u (gather=%d hunt=%d, "
	     "%u tiles known)\n",
	     c->player.zone_id, c->wave.gather_auto, c->wave.hunt_auto,
	     c->map_tile_count);
}
/* ------------------------------------------------------------------ */
/* Wave E: Guild Fest (ALLIANCEMOBILIZATION 3632..3644)                */
/*                                                                      */
/* Payloads from MobilizationManager (game client decompiled):          */
/*   DATA req (3632): empty                                             */
/*   DATA resp (3633):                                                  */
/*     u8 avail | u8 extra | u8 involved | u8 error |                   */
/*     20 x { u16 type; type==1001 ? i64 cd : u8 diff + 7 pad } |       */
/*     u8 moreRewards | u8 buyLimit | u16 extraPrize | u8 futureRank    */
/*     (every slot entry is exactly 10 bytes)                           */
/*   GET req (3637): u8 MissionPos (1-based)  -- start/query mission    */
/*   GET resp (3638): u8 err                                            */
/*     err 0   (accepted):  u16 id | u8 diff | u8 avail | i64 end |     */
/*                          u32 target                                  */
/*     err 255 (queried):   ... + i64 start  (status from target/max    */
/*                          needs the data table; DONE push covers it)  */
/*     err 4 = server busy, other = rejected                            */
/*   FINISH req (3641): empty -> resp (3642): u8 err (0 = claimed)      */
/*   UPDATE push (3643): u32 target (progress changed)                  */
/*   DONE push (3644): empty (mission complete -> claim with FINISH)    */
/*   REFLASH/BUY/DEL are NOT automated (reroll changes list,            */
/*   BUY costs diamonds, DEL abandons a running mission).               */
/* ------------------------------------------------------------------ */

#define GF_SLOTS 20

static struct {
	uint8_t  avail, extra, involved, err;
	uint16_t slot_type[GF_SLOTS + 1];  /* pos 1..20 */
	uint8_t  slot_diff[GF_SLOTS + 1];  /* 0xFE = CD slot, 0xFF = unusable */
	bool     data_valid;

	bool     has_mission;
	uint16_t mission_id;
	uint8_t  difficulty;
	int64_t  mission_end;
	uint32_t target;
	uint8_t  status;                   /* 0 running, 1 claimable, 2 expired */

	time_t   last_data, last_get, last_finish, last_start;
	uint8_t  finish_attempts;
} g_gf;

void WaveRecvGuildFestData(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size < 4 + GF_SLOTS * 10 + 5) {
		HexPreview("FEST_DATA(short)", data, size, 64);
		return;
	}

	g_gf.avail    = data[0];
	g_gf.extra    = data[1];
	g_gf.involved = data[2];
	g_gf.err      = data[3];

	uint16_t off = 4;
	int i;
	char slots[64];
	int p = 0;
	for (i = 1; i <= GF_SLOTS; i++) {
		uint16_t type = read_u16(data + off);
		uint8_t diff  = data[off + 2];
		g_gf.slot_type[i] = type;
		if (type == 0)
			g_gf.slot_diff[i] = 0xFF;
		else if (type == 1001)
			g_gf.slot_diff[i] = 0xFE;   /* CD / locked slot */
		else
			g_gf.slot_diff[i] = (diff > 3) ? 0xFF : diff;
		if (p < (int)sizeof(slots) - 8) {
			if (type == 0)
				p += snprintf(slots + p, sizeof(slots) - p, " -");
			else if (type == 1001)
				p += snprintf(slots + p, sizeof(slots) - p, " cd");
			else
				p += snprintf(slots + p, sizeof(slots) - p, " %u",
				              g_gf.slot_diff[i]);
		}
		off += 10;
	}

	g_gf.data_valid = true;
	g_gf.last_data  = time(NULL);
	LOGI("[FEST] list: avail=%u extra=%u members=%u err=%u "
	     "slot diffs[d0..d3]:%s\n",
	     g_gf.avail, g_gf.extra, g_gf.involved, g_gf.err, slots);
}

void WaveRecvGuildFestGet(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size < 1) {
		return;
	}
	uint8_t err = data[0];

	if (err == 0) {
		if (size < 17) {
			HexPreview("FEST_GET(short)", data, size, 32);
			return;
		}
		g_gf.mission_id   = read_u16(data + 1);
		g_gf.difficulty   = data[3];
		g_gf.avail        = data[4];
		g_gf.mission_end  = read_i64(data + 5);
		g_gf.target       = read_u32(data + 13);
		g_gf.has_mission  = (g_gf.mission_id != 0);
		g_gf.status       = 0;
		g_gf.finish_attempts = 0;
		g_gf.last_start   = time(NULL);
		LOGI("[FEST] START accepted: mission=%u diff=%u target=%u "
		     "end_in=%llds avail=%u\n",
		     g_gf.mission_id, g_gf.difficulty, g_gf.target,
		     (long long)(g_gf.mission_end - (int64_t)c->server_time),
		     g_gf.avail);
	} else if (err == 255) {
		if (size < 25) {
			HexPreview("FEST_QUERY(short)", data, size, 40);
			return;
		}
		g_gf.mission_id  = read_u16(data + 1);
		g_gf.difficulty  = data[3];
		g_gf.avail       = data[4];
		g_gf.mission_end = read_i64(data + 5);
		g_gf.target      = read_u32(data + 13);
		int64_t start    = read_i64(data + 17);
		if (g_gf.mission_id == 0) {
			g_gf.has_mission = false;
			g_gf.status = 0;
		} else {
			g_gf.has_mission = true;
			if (g_gf.status != 1 &&
			    g_gf.mission_end < (int64_t)c->server_time)
				g_gf.status = 2;
		}
		LOGI("[FEST] query: mission=%u diff=%u target=%u "
		     "end_in=%llds start_in=%llds status=%u\n",
		     g_gf.mission_id, g_gf.difficulty, g_gf.target,
		     (long long)(g_gf.mission_end - (int64_t)c->server_time),
		     (long long)(start - (int64_t)c->server_time),
		     g_gf.status);
	} else {
		LOGI("[FEST] GET error %u\n", err);
	}
}

void WaveRecvGuildFestFinish(Connection *c, const uint8_t *data,
                             uint16_t size)
{
	(void)c;
	if (size < 1) {
		return;
	}
	uint8_t err = data[0];
	if (err == 0) {
		LOGI("[FEST] FINISH accepted -- mission %u rewards claimed\n",
		     g_gf.mission_id);
		g_gf.has_mission = false;
		g_gf.mission_id  = 0;
		g_gf.status      = 0;
		g_gf.finish_attempts = 0;
		g_gf.last_finish = time(NULL);
	} else {
		LOGI("[FEST] FINISH error %u\n", err);
		g_gf.last_finish = time(NULL);
	}
}

void WaveRecvGuildFestUpdate(Connection *c, const uint8_t *data,
                             uint16_t size)
{
	(void)c;
	if (size < 4) {
		HexPreview("FEST_UPDATE(short)", data, size, 8);
		return;
	}
	g_gf.target = read_u32(data);
	LOGI("[FEST] progress: target=%u (mission %u diff %u)\n",
	     g_gf.target, g_gf.mission_id, g_gf.difficulty);
}

void WaveRecvGuildFestDone(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	(void)data;
	(void)size;
	g_gf.status = 1;
	LOGI("[FEST] DONE -- mission %u claimable (status=1)\n",
	     g_gf.mission_id);
}

static void GFestTick(Connection *c)
{
	if (!c->wave.guild_fest)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* Poll the mission list every 5 minutes (also serves as liveness). */
	if (!g_gf.data_valid || (now - g_gf.last_data) >= 300) {
		SendRaw(c, _MSG_REQUEST_ALLIANCEMOBLIZATION_MISSION_DATA,
		        NULL, 0);
		g_gf.last_data = now;
	}

	/* Claim a completed mission. */
	if (g_gf.has_mission && g_gf.status == 1 &&
	    (now - g_gf.last_finish) >= 15 && g_gf.finish_attempts < 5) {
		SendRaw(c, _MSG_REQUEST_ALLIANCEMOBLIZATION_MISSION_FINISH,
		        NULL, 0);
		g_gf.last_finish = now;
		g_gf.finish_attempts++;
		LOGI("[FEST] FINISH sent for mission %u (attempt %u)\n",
		     g_gf.mission_id, g_gf.finish_attempts);
		return;
	}

	/* Start the next mission when idle. */
	if (g_gf.has_mission || !g_gf.data_valid || g_gf.err != 0)
		return;
	if (g_gf.avail == 0)
		return;
	if (g_gf.last_start && (now - g_gf.last_start) < 20)
		return;

	uint8_t pref = c->wave.guildfest_difficulty;   /* 4 = any */
	int best = 0;
	uint8_t bestdiff = 0;
	int i;
	for (i = 1; i <= GF_SLOTS; i++) {
		uint8_t d = g_gf.slot_diff[i];
		if (d > 3)
			continue;                 /* empty or CD slot */
		if (pref <= 3 && d != pref)
			continue;
		if (best == 0 || d > bestdiff) {
			best = i;
			bestdiff = d;
		}
	}
	if (best == 0) {
		if (pref <= 3)
			LOGI("[FEST] no diff-%u slot available (avail=%u)\n",
			     pref, g_gf.avail);
		return;
	}

	uint8_t pos = (uint8_t)best;
	SendRaw(c, _MSG_REQUEST_ALLIANCEMOBLIZATION_MISSION_GET, &pos, 1);
	g_gf.last_start = now;
	LOGI("[FEST] GET pos=%u diff=%u (pref=%u) avail=%u\n",
	     pos, bestdiff, pref, g_gf.avail);
}

/* ------------------------------------------------------------------ */
/* Wave E: Labyrinth (GAMBLE 7001..7008, event push 3660)              */
/*                                                                      */
/* Payloads from GamblingManager (game client decompiled):              */
/*   INFO req (7001): empty                                             */
/*   INFO resp (7002): u8 err (0|1 ok) | u32 BigCost | u32 SmallCost |  */
/*     2 x { u8 stage, u8 remainFreePlay } | u32 prize                  */
/*     (GambleData[0] = Turbo/elite, [1] = Normal)                      */
/*   HIT req (7003): u8 mode (0 Turbo, 1 Normal)                        */
/*   HIT resp (7004): u8 result | u8 mode | u8 stage | u32 diamonds |   */
/*     u32 holyStars | u32 prize | u16 itemId | u16 itemNum |           */
/*     u8 itemRank | u8 remainFreePlay | u8 dailyFreeFlags              */
/*     result: 0 HIT, 1 DIE, 2 GREMLIN, 3 LEAVE, 4 SPECIAL              */
/*     (>100 = junk, ignore)                                            */
/*   PRIZE req (7005): empty -- UI polls it every 10s; resp (7006):     */
/*     u32 prize (pot refresh, NOT a claim)                             */
/*   PUSH 7008 HISTORY: 3 x { u16 kingdom, tag[3], name[13],            */
/*     u32 prizeWins, u8 gameType, i64 wonTime }                        */
/*   PUSH 3660 event: u32 sn | u8 state (1 = EAS_Run) | i64 begin |     */
/*     u32 require | u16 group | u16 monster                            */
/*                                                                      */
/* Hit policy: free combos first (remainFreePlay > 0), then one blind   */
/* daily-free probe per day; Holy Stars are spent only when             */
/* wave.labyrinth_spend=true.  Deduction is detected by comparing the   */
/* stars value from consecutive HIT responses.                          */
/* ------------------------------------------------------------------ */

static struct {
	bool     info_valid;
	uint32_t big_cost, small_cost;
	uint8_t  stage[2], free_play[2];
	uint32_t prize;

	bool     event_valid;
	uint32_t event_sn;
	uint8_t  event_state;              /* 1 = EAS_Run */
	int64_t  event_begin;
	uint32_t event_req;
	uint16_t event_group, monster_id;

	bool     stars_known;
	uint32_t stars;
	bool     paid_today;               /* paid hit seen today */
	bool     blind_tried;              /* daily-free probe used today */
	uint8_t  blind_rejects;            /* rejected probes today */
	time_t   blind_retry_at;           /* next allowed probe time */
	time_t   day_key;
	time_t   last_info, last_hit, last_prize, last_prize_resp;
	uint32_t last_prize_logged;
	bool     pending_paid;             /* current in-flight hit may cost */
} g_lab;

static const char *lab_result_name(uint8_t r)
{
	switch (r) {
	case 0:  return "HIT";
	case 1:  return "DIE";
	case 2:  return "GREMLIN";
	case 3:  return "LEAVE";
	case 4:  return "SPECIAL";
	default: return "?";
	}
}

void WaveRecvGambleInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size < 17) {
		HexPreview("LAB_INFO(short)", data, size, 32);
		return;
	}
	uint8_t err = data[0];
	if (err != 0 && err != 1) {
		LOGI("[LAB] info error %u\n", err);
		return;
	}
	g_lab.big_cost   = read_u32(data + 1);
	g_lab.small_cost = read_u32(data + 5);
	g_lab.stage[0]   = data[9];
	g_lab.free_play[0] = data[10];
	g_lab.stage[1]   = data[11];
	g_lab.free_play[1] = data[12];
	g_lab.prize      = read_u32(data + 13);
	g_lab.info_valid = true;
	g_lab.last_info  = time(NULL);
	LOGI("[LAB] info: cost elite=%u normal=%u stage[elite=%u normal=%u] "
	     "free[elite=%u normal=%u] prize=%u event=%s\n",
	     g_lab.big_cost, g_lab.small_cost,
	     g_lab.stage[0], g_lab.stage[1],
	     g_lab.free_play[0], g_lab.free_play[1], g_lab.prize,
	     !g_lab.event_valid ? "unknown"
	     : (g_lab.event_state == 1 ? "run" : "closed"));
}

void WaveRecvGambleStart(Connection *c, const uint8_t *data, uint16_t size)
{
	if (size < 22 || data[0] > 100 || data[1] > 1) {
		/* Server rejection: it answers 7004 with a 22-byte body of
		 * garbage when the hit is refused (no stars / no free hit /
		 * rate limit).  Nothing is granted or charged. */
		g_lab.pending_paid = false;
		g_lab.last_hit = time(NULL);
		g_lab.blind_rejects++;
		if (g_lab.blind_rejects < 8)
			g_lab.blind_retry_at = time(NULL) + 1800;
		LOGI("[LAB] hit rejected (junk 7004, rejects=%u/%u, next "
		     "probe in %ds)\n",
		     g_lab.blind_rejects, 8, 1800);
		HexPreview("LAB_HIT(rejected)", data, size, 24);
		return;
	}
	uint8_t result = data[0];
	uint8_t mode   = data[1];
	uint8_t  stage    = data[2];
	uint32_t diamonds = read_u32(data + 3);
	uint32_t stars    = read_u32(data + 7);
	uint32_t prize    = read_u32(data + 11);
	uint16_t item_id  = read_u16(data + 15);
	uint16_t item_num = read_u16(data + 17);
	uint8_t  item_rank = data[19];
	uint8_t  free_new  = data[20];
	uint8_t  daily_free = data[21];

	bool paid = false;
	if (g_lab.pending_paid && g_lab.stars_known && stars < g_lab.stars) {
		paid = true;
		if (!c->wave.labyrinth_spend) {
			g_lab.paid_today = true;
			LOGI("[LAB] PAID hit! stars %u -> %u (-%u), "
			     "free-only mode: stopping paid hits today\n",
			     g_lab.stars, stars, g_lab.stars - stars);
		} else {
			LOGI("[LAB] paid hit: stars %u -> %u (-%u)\n",
			     g_lab.stars, stars, g_lab.stars - stars);
		}
	}
	g_lab.stars_known = true;
	g_lab.stars = stars;
	g_lab.stage[mode] = stage;
	g_lab.free_play[mode] = free_new;
	g_lab.prize = prize;
	g_lab.pending_paid = false;
	g_lab.last_hit = time(NULL);
	g_lab.blind_rejects = 0;
	g_lab.blind_retry_at = 0;
	/* dailyFree bit0 = Turbo free spent, bit1 = Normal free spent. */
	if ((mode == 1 && (daily_free & 2)) || (mode == 0 && (daily_free & 1)))
		g_lab.blind_tried = true;

	LOGI("[LAB] %s mode=%s stage=%u free=%u stars=%u%s "
	     "diamonds=+%u prize=%u item=%u x%u r%u dailyFree=%u\n",
	     lab_result_name(result),
	     mode == 1 ? "normal" : "elite", stage, free_new, stars,
	     paid ? "(paid)" : "", diamonds, prize, item_id, item_num,
	     item_rank, daily_free);
}

void WaveRecvGamblePrize(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	if (size < 4) {
		HexPreview("LAB_PRIZE(short)", data, size, 8);
		return;
	}
	g_lab.prize = read_u32(data);
	g_lab.last_prize_resp = time(NULL);
	if (g_lab.prize != g_lab.last_prize_logged) {
		g_lab.last_prize_logged = g_lab.prize;
		LOGI("[LAB] prize pot = %u\n", g_lab.prize);
	}
}

void WaveRecvGambleUpdateInfo(Connection *c, const uint8_t *data,
                              uint16_t size)
{
	if (size < 21) {
		HexPreview("LAB_EVENT(short)", data, size, 24);
		return;
	}
	uint32_t sn     = read_u32(data);
	uint8_t  state  = data[4];
	int64_t  begin  = read_i64(data + 5);
	uint32_t req    = read_u32(data + 13);
	uint16_t group  = read_u16(data + 17);
	uint16_t monster = read_u16(data + 19);

	bool sn_changed = (!g_lab.event_valid || g_lab.event_sn != sn);
	g_lab.event_valid = true;
	g_lab.event_sn = sn;
	g_lab.event_state = state;
	g_lab.event_begin = begin;
	g_lab.event_req = req;
	g_lab.event_group = group;
	g_lab.monster_id = monster;

	if (state == 1) {
		LOGI("[LAB] event RUN sn=%u monster=%u group=%u "
		     "open_in=%llds lasts=%us\n",
		     sn, monster, group,
		     (long long)(begin - (int64_t)c->server_time), req);
	} else {
		LOGI("[LAB] event state=%u (not running) monster=%u "
		     "group=%u\n", state, monster, group);
	}
	if (sn_changed && c->wave.labyrinth)
		SendRaw(c, _MSG_REQUEST_GAMBLE_INFO, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Wave E: Kingdom Tycoon (MONOPOLY 7010..7020, event 7030..7040)      */
/*                                                                      */
/* Same free-only discipline as the Labyrinth: exactly one free roll a  */
/* day, never a Luck Token (600-720 gems each in the store).            */
/*   INFO  req 7010 empty                                               */
/*   STEP  req 7012 empty — roll/stop the die                          */
/*   CRYSTAL req 7014 — open a Gremlin capsule (3 per gremlin)         */
/* Layout is not yet confirmed from a live client, so this tick stays  */
/* conservative: it polls INFO, dumps it, and only sends STEP when the */
/* server says a free roll is available. If the parse turns out to be  */
/* wrong the worst case is a rejected request, never a purchase.       */
/* ------------------------------------------------------------------ */

static struct {
	bool     info_valid;
	bool     free_roll;
	uint8_t  crystals;
	uint32_t jackpot;
	time_t   last_info;
	time_t   last_step;
	time_t   last_crystal;
} g_tycoon = {0};

void WaveRecvMonopolyInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("MONOPOLY_INFO", data, size, 48);
	if (size < 2)
		return;

	g_tycoon.info_valid = true;
	/* Best-effort: treat a small non-zero "free/token" field as an
	 * available roll. The exact offsets get confirmed from the dump
	 * on the next run; until then we only ever *report* the state. */
	g_tycoon.free_roll = (data[1] != 0);
	g_tycoon.crystals  = (size > 2) ? data[2] : 0;

	LOGI("[TYCOON] info size=%u free_roll=%u crystals=%u "
	     "(free-only: never buys a Luck Token)\n",
	     size, g_tycoon.free_roll, g_tycoon.crystals);

	g_tycoon.last_info = time(NULL);
}

void WaveRecvMonopolyStep(Connection *c, const uint8_t *data, uint16_t size)
{
	HexPreview("MONOPOLY_STEP", data, size, 48);
	if (size > 0 && data[0] == 0) {
		LOGI("[TYCOON] roll accepted (size=%u)\n", size);
		/* A successful roll can drop a Gremlin capsule. */
		SendRaw(c, _MSG_REQUEST_MONOPOLY_CRYSTAL, NULL, 0);
		g_tycoon.last_crystal = time(NULL);
	} else if (size > 0) {
		LOGI("[TYCOON] roll rejected (err=%u)\n", data[0]);
	}
}

static void TycoonTick(Connection *c)
{
	if (!c->wave.tycoon)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	if (!g_tycoon.info_valid || (now - g_tycoon.last_info) >= 300) {
		SendRaw(c, _MSG_REQUEST_MONOPOLY_INFO, NULL, 0);
		g_tycoon.last_info = now;
	}

	if (!g_tycoon.info_valid || !g_tycoon.free_roll)
		return;
	if ((now - g_tycoon.last_step) < 30)
		return;

	g_tycoon.last_step = now;
	SendRaw(c, _MSG_REQUEST_MONOPOLY_STEP, NULL, 0);
	LOGI("[TYCOON] free daily roll requested\n");
}

static void LabTick(Connection *c)
{
	if (!c->wave.labyrinth)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

	/* Day rollover (local time): re-arm the daily-free probe. */
	time_t day = now / 86400;
	if (g_lab.day_key != day) {
		g_lab.day_key = day;
		g_lab.paid_today = false;
		g_lab.blind_tried = false;
		g_lab.blind_rejects = 0;
		g_lab.blind_retry_at = 0;
	}

	/* Info poll every 2 minutes (also the first request). */
	if (!g_lab.info_valid || (now - g_lab.last_info) >= 120) {
		SendRaw(c, _MSG_REQUEST_GAMBLE_INFO, NULL, 0);
		g_lab.last_info = now;
	}

	/* Prize pot refresh (game polls every 10s; we are lazier). */
	if (g_lab.info_valid && (now - g_lab.last_prize) >= 60) {
		SendRaw(c, _MSG_REQUEST_GAMBLE_PRIZE, NULL, 0);
		g_lab.last_prize = now;
	}

	if (!g_lab.info_valid)
		return;
	if (g_lab.event_valid && g_lab.event_state != 1)
		return;                       /* event closed */
	if ((now - g_lab.last_hit) < 10)
		return;

	uint8_t mode = c->wave.labyrinth_mode ? 1 : 0;
	uint32_t cost = (mode == 1) ? g_lab.small_cost : g_lab.big_cost;
	bool free = g_lab.free_play[mode] > 0;
	bool allow = false;
	bool mark_paid = false;

	if (free) {
		allow = true;                 /* combo / Divine Blessing */
	} else if (c->wave.labyrinth_spend) {
		if (!g_lab.stars_known || g_lab.stars >= cost) {
			allow = true;
			mark_paid = true;
		} else {
			LOGI("[LAB] hold: need %u stars, have %u\n",
			     cost, g_lab.stars);
		}
	} else if (!g_lab.paid_today && g_lab.blind_rejects < 8) {
		/* Blind daily-free probe.  First try once per day; after a
		 * rejection keep retrying every 30 min while we cannot
		 * afford the paid hit anyway (stars < cost), so the daily
		 * free window is caught without any charge risk. */
		bool stars_safe = !g_lab.stars_known || g_lab.stars < cost;
		if (!g_lab.blind_tried) {
			allow = true;
			mark_paid = true;
			g_lab.blind_tried = true;
		} else if (stars_safe && g_lab.blind_retry_at &&
		           now >= g_lab.blind_retry_at) {
			allow = true;
			mark_paid = true;
			g_lab.blind_retry_at = now + 1800;
		}
	}

	if (!allow)
		return;

	uint8_t m = mode;
	SendRaw(c, _MSG_REQUEST_GAMBLE_STARTGAME, &m, 1);
	g_lab.last_hit = now;
	g_lab.pending_paid = mark_paid;
	LOGI("[LAB] HIT mode=%s cost=%u free=%d stars=%s\n",
	     mode == 1 ? "normal" : "elite", cost, free,
	     g_lab.stars_known ? "tracked" : "?");
}

/* ------------------------------------------------------------------ */
/* Wave E: hero stage quick battle (1805 / 1806)                       */
/*                                                                      */
/* QUICKBATTLE answered an empty 1805 with a 502-byte payload          */
/* (live).  Semantics (query vs sweep) are still under analysis:        */
/* the full hex dump lands in the log while wave.stage_sweep is on,     */
/* then the payload is wired from evidence.                             */
/* ------------------------------------------------------------------ */

static time_t g_stage_last = 0;

void WaveRecvQuickBattle(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	LOGI("[STAGE] QUICKBATTLE resp size=%u\n", size);
	HexPreview("STAGE_SWEEP", data, size, 512);
}

static void StageTick(Connection *c)
{
	if (!c->wave.stage_sweep)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_stage_last && (now - g_stage_last) < 300)
		return;
	g_stage_last = now;

	SendRaw(c, _MSG_REQUEST_QUICKBATTLE, NULL, 0);
	LOGI("[STAGE] QUICKBATTLE(1805) sent (BETA, empty payload)\n");
}

/* ------------------------------------------------------------------ */
/* probe / status                                                      */
/* ------------------------------------------------------------------ */

bool ProbePacket(Connection *c, const char *args)
{
	if (!args || !*args)
		return false;

	char *end = NULL;
	long opcode = strtol(args, &end, 0);

	if (opcode <= 0 || opcode > 65535 || end == args) {
		LOGI("[PROBE] bad opcode: %s\n", args);
		return false;
	}

	while (*end == ' ')
		end++;

	uint8_t payload[256];
	uint16_t len = 0;

	while (*end) {
		while (*end == ' ')
			end++;
		if (!*end)
			break;

		char byte_str[3] = { end[0], end[1] ? end[1] : '\0', '\0' };
		if (!byte_str[1]) {
			LOGI("[PROBE] odd hex digit near: %s\n", end);
			return false;
		}
		char *bend = NULL;
		long byte = strtol(byte_str, &bend, 16);
		if (bend != byte_str + 2) {
			LOGI("[PROBE] bad hex pair: %s\n", byte_str);
			return false;
		}
		if (len >= sizeof(payload)) {
			LOGI("[PROBE] payload too long (max %u)\n",
			     (unsigned)sizeof(payload));
			return false;
		}
		payload[len++] = (uint8_t)byte;
		end += 2;
	}

	SendRaw(c, (uint16_t)opcode, payload, len);

	char hex[3 * 64 + 4];
	uint16_t pos = 0;
	for (uint16_t i = 0; i < len && pos + 3 < sizeof(hex); i++)
		pos += (uint16_t)snprintf(hex + pos, sizeof(hex) - pos,
		                          "%02X ", payload[i]);

	LOGI("[PROBE] sent opcode=%ld (0x%04X) len=%u payload=[%s] "
	     "(watch for response 0x%04X with wave.log_packets=true)\n",
	     opcode, (unsigned)opcode, len, len ? hex : "",
	     (unsigned)(opcode + 1));
	return true;
}

void WaveStatusMail(Connection *c, const char *player_name)
{
	char body[2048];
	int n = 0;

#define ST(b) ((b) ? "ON" : "off")
	n += snprintf(body + n, sizeof(body) - n,
		"WAVE STATUS\n"
		"-----------\n"
		"B: build.upgrade=%s  research=%s  shelter(atk/scout)=%s/%s\n"
		"   quest=%s  vip=%s  arena=%s\n"
		"C: gather=%s  hunt=%s (max lvl %u, scan %us)\n"
		"D: trap=%s  pet=%s  rewards=%s (mask 0x%02X)\n"
		"E: fest=%s avail=%u mission=%u st=%u | lab=%s %s "
		"free=[%u,%u] stars=%s%u | sweep=%s\n"
		"F: heal=%s style=%u slots=%u treat=%u | heroes=%u "
		"marches=%u/%u\n"
		"dbg: log_packets=%s\n"
		"buildings=%u queue=%s research_tech=%u\n"
		"army total=%u tiers=%u  wounded=%u\n"
		"\n"
		"PAYLOADS: game-decompiled (research u16+i32, shelter,\n"
		"  quests 3113/3117/3119, arena 5208 full, trap 2605,\n"
		"  guild fest 3632-3644, labyrinth 7001-7008+3660,\n"
		"  scout/march/monster structures known).\n"
		"LIVE-VALIDATED: build.upgrade, shelter(5603), arena\n"
		"  board(5209), vip(3125), treasure-daily(3136),\n"
		"  daily reward(11873), mission info(3112),\n"
		"  lab info(7002), fest list(3633), quickbattle(1806)\n"
		"UNVERIFIED: research effect (needs post-start RESEARCHINFO),\n"
		"  quest start/finish/complete responses, arena targets\n"
		"  (5201/5206 push), trap construct, march/gather/hunt,\n"
		"  pet train, fest GET/FINISH, lab hits, stage sweep\n",
		ST(c->wave.build_upgrade), ST(c->wave.research_auto),
		ST(c->wave.shelter_on_attack), ST(c->wave.shelter_on_scout),
		ST(c->wave.quest_claim), ST(c->wave.vip_collect),
		ST(c->wave.arena_challenge),
		ST(c->wave.gather_auto), ST(c->wave.hunt_auto),
		c->wave.hunt_max_level, c->wave.scan_interval_s,
		ST(c->wave.trap_build), ST(c->wave.pet_train),
		ST(c->wave.reward_claim), c->wave.reward_mask,
		ST(c->wave.guild_fest), g_gf.avail, g_gf.mission_id,
		g_gf.status,
		ST(c->wave.labyrinth),
		!g_lab.event_valid ? "unknown"
		: (g_lab.event_state == 1 ? "run" : "closed"),
		g_lab.free_play[0], g_lab.free_play[1],
		g_lab.stars_known ? "" : "?", g_lab.stars,
		ST(c->wave.stage_sweep),
		ST(c->wave.heal_troops), c->wave.heal_style,
		c->wounded.troop.total, c->wounded.healing.total,
		c->hero_count, c->player.current_marches,
		c->player.max_marches,
		ST(c->wave.log_packets),
		c->building_count,
		queue_idle(&c->build_queue, (int64_t)c->server_time)
			? "idle" : "busy",
		c->technology.research_tech,
		c->troop.total, (unsigned)c->troop.tiers,
		c->wounded.troop.total);
#undef ST

	if (n > 0)
		RequestSendMailFmt(c, player_name, "Wave Status", "%s", body);
}

/* ------------------------------------------------------------------ */
/* master tick                                                         */
/* ------------------------------------------------------------------ */

static time_t g_wave_last = 0;

void WaveTick(Connection *c)
{
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	if (g_wave_last && (now - g_wave_last) < 10)
		return;
	g_wave_last = now;

	/* Free-only priority ladder (from lordsmobile.fandom.com + the
	 * commercial bots' documented ordering). Highest ROI first:
	 *   1. claim idle free rewards
	 *   2. guild help (sending + asking on our own builds)
	 *   3. keep the construction queue busy
	 *   4. keep the research queue busy
	 *   5. keep training + gathering marches busy
	 *   6/7. free energy -> hunts, free stamina -> hero stages
	 *   8. quest claims
	 *  10. timed events (colosseum payout, labyrinth, tycoon, cargo)
	 * Every one of these uses free resources, free VIP acceleration,
	 * guild help or earned items only — never gems. */
	RewardTick(c);        /* 1  free rewards / treasure / daily    */
	AllianceGiftTick(c);  /* 1  guild gifts (24h expiry, 300 cap)  */
	VipTick(c);           /* 1  VIP quest chest (1h)              */
	OnlineGiftTick(c);    /* 1  free Turf box (~15 min)           */
	QuestTick(c);         /* 8  daily / admin / guild quests      */

	BuildTick(c);         /* 3  construction queue (+ 2852 help)   */
	ResearchTick(c);      /* 4  research queue                     */

	MapScanTick(c);       /* 5  map intel for gathering            */
	if (GatherTick(c))    /* 5  gathering marches, fit to tile     */
		g_hunt_last = time(NULL);   /* army is out - defer hunt */
	else
		HuntTick(c);       /* 6  free energy -> monster hunt       */

	StageTick(c);         /* 7  free stamina -> hero stage sweep   */
	ArenaTick(c);         /* 10 colosseum (free entries only)     */
	LabTick(c);           /* 10 labyrinth free daily hit          */
	TycoonTick(c);        /* 10 kingdom tycoon free daily roll    */
	GFestTick(c);         /* 10 guild fest scoring                */

	ShelterTick(c);       /* defense: keep the shelter window alive */
	TrapTick(c);          /* defense upkeep                        */
	PetTick(c);           /* familiar training                     */
	HealTick(c);          /* infirmary (free only)                 */
}
