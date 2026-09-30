/*
 * Wave A automations: automatic troop training and automatic speed-ups.
 *
 * Both features are driven by configuration keys parsed in config.c
 * (train.* and speedup.*) and are OFF by default. Every activation is
 * logged so the web dashboard log shows exactly when they fire.
 */

#include "connection.h"
#include "protocol.h"
#include "items.h"
#include "log.h"

#include <time.h>

static time_t g_train_last = 0;
static time_t g_speedup_last = 0;
static time_t g_speedup_day = 0;
static uint16_t g_speedup_used = 0;
static int64_t g_speedup_skip_log = -1;

static const char *TroopKindName(uint8_t kind)
{
	switch (kind) {
		case 0: return "infantry";
		case 1: return "ranged";
		case 2: return "cavalry";
		case 3: return "siege";
		default: return "?";
	}
}

/* Dismiss surplus troops.
 *
 * Training targets are per-kind totals, so the scheduler tops up toward
 * them; what it does not do is shrink back. A surplus above the
 * configured ceiling is exactly what dies when the account is hit, and
 * wounded troops overflow the infirmary into the sanctuary.
 *
 * Guards, all of which matter:
 *  - only when the queue is idle and nothing is in flight to a march
 *  - never below the configured target (the target IS the ceiling)
 *  - per-request cap so one dismiss is a correction, not a purge
 *  - dismissed amounts are credited locally so the next cycle does not
 *    re-dismiss the same troops
 */
static time_t g_dismiss_last = 0;

void DismissTick(Connection *c)
{
	if (!c->train.dismiss_above)
		return;
	if (c->server_time == 0)
		return;
	if (!c->troop.loaded)
		return;

	time_t now = time(NULL);
	if (g_dismiss_last && (now - g_dismiss_last) < 900)
		return;

	/* Never dismiss while troops are marching: the server count and the
	 * on-map force differ, and dismissing a marching force is rejected. */
	if (c->player.current_marches > 0)
		return;
	if (c->train.pending_qty > 0)
		return;

	for (uint8_t kind = 0; kind < 4; kind++) {
		uint32_t ceiling = c->train.target_total[kind];
		if (ceiling == 0)
			continue;

		uint64_t have = 0;
		for (uint8_t t = 0; t < 4; t++)
			have += c->troop.kinds[kind][t];
		if (have <= ceiling)
			continue;

		uint32_t excess = (uint32_t)(have - ceiling);
		/* Correct the biggest tier first — it carries the most
		 * upkeep and the worst wound/death maths. */
		for (int t = 3; t >= 0 && excess > 0; t--) {
			uint32_t n = c->troop.kinds[kind][t];
			if (n == 0)
				continue;
			if (n > excess)
				n = excess;
			if (c->train.dismiss_batch && n > c->train.dismiss_batch)
				n = c->train.dismiss_batch;
			if (n == 0)
				continue;

			RequestTroopDismiss(c, kind, (uint8_t)(t + 1), n);
			c->troop.kinds[kind][t] -= n;
			if (t < 4) {
				uint32_t *arr[4] = { c->troop.infantry,
					c->troop.ranged, c->troop.cavalry,
					c->troop.siege };
				arr[kind][t] = c->troop.kinds[kind][t];
			}
			if (c->troop.total >= n)
				c->troop.total -= n;
			excess -= n;
			g_dismiss_last = now;
			LOGI("[TRAIN] dismiss %s T%u x%u (have %llu > ceiling "
			     "%u)\n", TroopKindName(kind), t + 1, n,
			     (unsigned long long)have, ceiling);
			return;   /* one correction per cycle */
		}
	}
}

void TrainingTick(Connection *c)
{
	if (!c->train.enabled)
		return;

	if (c->server_time == 0)
		return;

	/* Pre-login requests get err=1 from the server — wait for the
	 * role info so the account name is known. */
	if (c->player.name[0] == '\0')
		return;

	time_t now = time(NULL);
	uint32_t interval = c->train.interval_s ? c->train.interval_s : 120;

	/* Anti-ban jitter: +/- 15% of the configured interval. */
	interval = interval - (uint32_t)(interval * 15 / 100) +
	           (uint32_t)((now * 7919u) % (uint32_t)(interval * 30 / 100 + 1));

	if (g_train_last && (now - g_train_last) < (time_t)interval)
		return;

	/* Queue still cooking — don't hammer the server with starts that
	 * it answers err=1. Re-arm after the server timer (+60s grace),
	 * or after a fixed window if the timer is unknown (login seed).
	 * A generous window also self-heals a lost ADDSOLDIER push. */
	if (c->train.pending_qty > 0 && c->train.pending_since > 0) {
		int64_t wait = c->train.instant_finish ? 120 :
		               (c->train.pending_need > 0 ?
		                (int64_t)c->train.pending_need + 60 : 900);
		if (now < c->train.pending_since + wait)
			return;
		LOGI("[TRAIN] pending %u x kind=%u overdue (need=%us) — "
		     "re-arming\n",
		     c->train.pending_qty, c->train.pending_kind,
		     c->train.pending_need);
	}

	g_train_last = now;

	/* Kind selection.
	 *
	 * Training one fixed kind forever produces a mono-army that any
	 * player reads instantly and counters for free (inf -> ranged ->
	 * cav -> inf). The reference bot's "Rotate Troops" cycles a chosen
	 * order instead. With no rotation configured we keep the single
	 * configured kind, so existing configs are unchanged. */
	uint8_t kind = c->train.kind;
	if (c->train.rotate_count > 0)
		kind = c->train.rotate_kind[c->train.rotate_cursor %
		                            c->train.rotate_count];

	/* Target totals.
	 *
	 * The configured amount is a *total including already-trained*
	 * troops, not a per-batch increment, so a partially-trained army
	 * gets topped up instead of over-trained. This is also what keeps
	 * the barracks within its capacity and stops the surplus becoming
	 * dead troops on the first real hit. */
	uint32_t target = c->train.target_total[kind]
	                  ? c->train.target_total[kind]
	                  : c->train.amount;
	if (target == 0)
		target = 1000;

	uint64_t have = 0;
	for (uint8_t t = 0; t < 4; t++)
		have += c->troop.kinds[kind][t];

	if (have >= target) {
		/* This kind is satisfied — move to the next one in the
		 * rotation rather than queueing a pointless batch. */
		if (c->train.rotate_count > 0) {
			c->train.rotate_cursor =
				(uint8_t)((c->train.rotate_cursor + 1) %
				          c->train.rotate_count);
			g_train_last = 0;   /* re-evaluate on the next pass */
			LOGI("[TRAIN] %s at target (%llu/%u) — rotating\n",
			     TroopKindName(kind), (unsigned long long)have,
			     target);
		}
		return;
	}

	uint32_t amount = (uint32_t)(target - have);
	if (amount > c->train.max_batch && c->train.max_batch)
		amount = c->train.max_batch;
	if (amount == 0)
		return;

	RequestTroopTraining(c, kind, c->train.tier, amount);
	LOGI("[TRAIN] auto-train kind=%u (%s) tier=%u amount=%u "
	     "(have %llu / target %u, interval~%us)\n",
	     kind, TroopKindName(kind), (unsigned)c->train.tier,
	     amount, (unsigned long long)have, target, interval);

	/* Advance the rotation on a confirmed start so the next batch
	 * trains a different type. */
	if (c->train.rotate_count > 0)
		c->train.rotate_cursor = (uint8_t)((c->train.rotate_cursor + 1) %
		                                   c->train.rotate_count);
}

void SpeedupTick(Connection *c)
{
	if (!c->speedup.enabled)
		return;

	if (c->server_time == 0)
		return;

	int64_t now = (int64_t)c->server_time;
	bool active = false;
	const char *what = NULL;

	/* Active construction? */
	if (c->build_queue.total_time > 0) {
		int64_t end = c->build_queue.start_time + (int64_t)c->build_queue.total_time;
		if (end > now) {
			active = true;
			what = "construction";
		}
	}

	/* Active research? */
	/* Active research? finish_time is the START instant, so the
	 * end is start + total_time (same fix as research_idle()). */
	if (!active && c->technology.total_time > 0 &&
	    c->technology.finish_time + (int64_t)c->technology.total_time > now) {
		active = true;
		what = "research";
	}

	if (!active)
		return;

	time_t wall = time(NULL);

	/* Rate limit: at most one speed-up per minute. */
	if (g_speedup_last && (wall - g_speedup_last) < 60)
		return;

	/* Daily cap, resets every 24h. */
	if (g_speedup_day == 0 || (wall - g_speedup_day) >= 86400) {
		g_speedup_day = wall;
		g_speedup_used = 0;
	}

	uint16_t cap = c->speedup.daily_cap ? c->speedup.daily_cap : 20;
	if (g_speedup_used >= cap)
		return;

	/* Don't waste a partial item.
	 * A 30-minute item on a job that has 20 minutes left is pure loss,
	 * and the old code fired one every 60s regardless — burning up to
	 * 10h of earned speed-ups a day. Only use the item when it is
	 * actually covered by the remaining queue time (with a small
	 * margin), and prefer research speed-ups for a research queue. */
	const uint32_t ITEM_SECONDS = 30 * 60;
	int64_t end = 0;
	if (c->build_queue.total_time > 0)
		end = c->build_queue.start_time + (int64_t)c->build_queue.total_time;
	else if (c->technology.total_time > 0)
		end = c->technology.finish_time + (int64_t)c->technology.total_time;

	int64_t remaining = end - now;
	if (remaining <= (int64_t)ITEM_SECONDS) {
		if (g_speedup_skip_log != (int64_t)remaining) {
			g_speedup_skip_log = (int64_t)remaining;
			LOGI("[SPEEDUP] holding: %llds left on %s, a 30-min item "
			     "would be wasted\n", (long long)remaining,
			     what ? what : "task");
		}
		return;
	}

	/* Only use speed-ups we actually own. */
	if (c->items[SPEED_UP_30_MINUTE].quantity == 0) {
		return;
	}

	g_speedup_used++;
	g_speedup_last = wall;

	RequestSimpleUseItem(c, SPEED_UP_30_MINUTE, 1);
	LOGI("[SPEEDUP] using 30-min speedup on %s, %llds remaining "
	     "(%u/%u today)\n",
	     what ? what : "task", (long long)remaining,
	     (unsigned)g_speedup_used, (unsigned)cap);
}
