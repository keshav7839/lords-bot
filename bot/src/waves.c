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

static void SendRaw(Connection *c, uint16_t opcode,
                    const uint8_t *payload, uint16_t len)
{
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


static bool queue_idle(const BuildQueueInfo *q, int64_t server_time)
{
	if (q->total_time == 0)
		return true;
	return (int64_t)(q->start_time + (int64_t)q->total_time) <= server_time;
}

static bool research_idle(const TechnologyInfo *t, int64_t server_time)
{
	if (t->total_time == 0)
		return true;
	return t->finish_time <= server_time;
}

/* ------------------------------------------------------------------ */
/* Wave B: automatic building upgrade                                 */
/* ------------------------------------------------------------------ */

static time_t g_build_last = 0;
static time_t g_build_err_logged = 0;
static uint8_t g_build_cursor = 0;
static bool    g_build_pending = false; /* waiting for BUILDBEGIN/ERROR */

static const uint16_t default_build_priority[] = {
	BUILD_CASTLE, BUILD_ACADEMY, BUILD_WALL, BUILD_MANOR, BUILD_VAULT,
	BUILD_INFIRMARY, BUILD_BARRACKS, BUILD_TRADING_POST, BUILD_WATCHTOWER,
	BUILD_EMBASSY, BUILD_WORKSHOP, BUILD_TIMBER, BUILD_STONE, BUILD_ORE,
	BUILD_FOOD
};

void WaveUpgradeOne(Connection *c)
{
	time_t now = time(NULL);

	if (c->server_time == 0)
		return;
	if (c->building_count == 0) {
		LOGI("[BUILD] no building data yet, waiting\n");
		return;
	}
	if (!queue_idle(&c->build_queue, (int64_t)c->server_time)) {
		LOGI("[BUILD] queue busy (pos %u)\n", c->build_queue.position);
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

static void ArenaTick(Connection *c)
{
	if (!c->wave.arena_challenge)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);

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
		write_u16(payload + pos, g_arena.def_hero[i]);
		pos += 2;
	}
	SendRaw(c, _MSG_REQUEST_ARENA_CHALLENGE, payload, pos);
	LOGI("[ARENA] challenging rank %u (28-byte game payload, heroes from "
	     "defense squad)\n", g_arena.t_place);
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
/* Wave D: traps (learning mode)                                       */
/* ------------------------------------------------------------------ */

static time_t g_trap_last = 0;

void WaveRecvTrapInfo(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview("TRAPINFO", data, size, 48);
}

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

	/* Game payload (UIBarrack_Soldier.SendTrapConstruct):
	 *   u8 (RD_Kind - 4) | u8 (RD_Rank - 1) | u32 count
	 * kind 0..11 = trap types, rank 0..3 = tiers.  Small count so a
	 * resource miss only rejects harmlessly; response = 2606. */
	uint8_t payload[6];
	payload[0] = 0;                       /* trap type 0 */
	payload[1] = 0;                       /* rank T1 */
	write_u32(payload + 2, 1);            /* quantity */
	SendRaw(c, _MSG_REQUEST_TRAPCONSTRUCT, payload, 6);
	LOGI("[TRAP] construct kind=0 rank=0 count=1 (game payload, resp 2606)\n");
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
/* Wave C: map scanning (gather / monster hunt learning mode)          */
/* ------------------------------------------------------------------ */

static time_t g_scan_last = 0;

void WaveRecvMapUpdate(Connection *c, const uint8_t *data, uint16_t size)
{
	(void)c;
	HexPreview("MAP_UPDATE", data, size, 48);
}

static void MapScanTick(Connection *c)
{
	if (!c->wave.gather_auto && !c->wave.hunt_auto)
		return;
	if (c->server_time == 0)
		return;

	time_t now = time(NULL);
	uint32_t interval = c->wave.scan_interval_s ? c->wave.scan_interval_s
	                                             : 300;
	if (g_scan_last && (now - g_scan_last) < (time_t)interval)
		return;
	g_scan_last = now;

	/* Request map data around home zone.  Responses
	 * (UPDATE_MAPINFO / UPDATE_MAPINFO_PLUS) are logged so tile
	 * layouts can be parsed; march sending (TROOPMARCH 2415 /
	 * SENDMONSTER 2488) is wired after that. */
	uint16_t zones[4] = { c->player.zone_id, 0, 0, 0 };
	RequestMapData(c, 1, zones);
	LOGI("[SCAN] map data requested for zone %u "
	     "(gather=%d hunt=%d learning mode)\n",
	     c->player.zone_id, c->wave.gather_auto, c->wave.hunt_auto);
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

	BuildTick(c);
	ResearchTick(c);
	QuestTick(c);
	VipTick(c);
	ArenaTick(c);
	RewardTick(c);
	TrapTick(c);
	PetTick(c);
	MapScanTick(c);
	GFestTick(c);
	LabTick(c);
	StageTick(c);
}
