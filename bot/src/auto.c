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

	uint32_t amount = c->train.amount ? c->train.amount : 1000;

	RequestTroopTraining(c, c->train.kind, c->train.tier, amount);
	LOGI("[TRAIN] auto-train kind=%u tier=%u amount=%u (interval~%us)\n",
	     (unsigned)c->train.kind, (unsigned)c->train.tier,
	     amount, interval);
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
