/*
 * Configuration parser.
 *
 * Each configuration key is matched using a simple if/return chain.
 *
 * Although a lookup table or hash map could also be used, this function is
 * only executed once during application startup when the configuration file
 * is loaded. It is never called from the main game loop, network packet
 * processing, or any performance-critical code.
 *
 * Because the parser runs only once, the cost of multiple strcmp() calls is
 * negligible compared to the overall initialization time. This approach keeps
 * the code straightforward, easy to read, and simple to extend—adding a new
 * configuration option only requires adding another comparison.
 *
 * Prefer clarity and maintainability here over unnecessary optimization.
 */

#include "config.h"
#include <stdlib.h>

#include <ctype.h>

static CommandChannel ParseCommandChannel(const char *value)
{
    if (strcmp(value, "WORLD") == 0)
        return COMMAND_CHANNEL_WORLD;

    if (strcmp(value, "GUILD") == 0)
        return COMMAND_CHANNEL_GUILD;

    if (strcmp(value, "MAIL") == 0)
        return COMMAND_CHANNEL_MAIL;

    return COMMAND_CHANNEL_GUILD; // default
}


static uint16_t ParseShield(const char *value)
{
    if (strcmp(value, "SHIELD_4H") == 0)
        return SHIELD_4H;

    if (strcmp(value, "SHIELD_8H") == 0)
        return SHIELD_8H;

    if (strcmp(value, "SHIELD_12H") == 0)
        return SHIELD_12H;

    if (strcmp(value, "SHIELD_1D") == 0)
        return SHIELD_1D;

    if (strcmp(value, "SHIELD_3D") == 0)
        return SHIELD_3D;

    if (strcmp(value, "SHIELD_7D") == 0)
        return SHIELD_7D;

    if (strcmp(value, "SHIELD_14D") == 0)
        return SHIELD_14D;

    return 0;
}

static bool ParseShieldPriority(Connection *c, const char *value)
{
    char buffer[512];

    strncpy(buffer, value, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;

    char *token = strtok(buffer, ",");
    
    while (token && count < 8) {
        while (*token == ' ')
            token++;

        uint16_t shield = ParseShield(token);

        if (shield == 0) {
            printf("Invalid shield priority value: %s\n", token);
            return false;
        }

        c->protection.shield_priority[count++] = shield;

        token = strtok(NULL, ",");
    }

    c->protection.shield_priority_count = count;
    return true;
}

/*
uint64_t parse_number_u64(const char *str) {
    double value = 0.0;
    char suffix = '\0';

    // Read numeric part and optional suffix
    sscanf(str, "%lf%c", &value, &suffix);
    suffix = tolower(suffix); // handle both lowercase and uppercase

    // Apply multiplier
    switch (suffix) {
        case 'k': value *= 1000ULL; break;
        case 'm': value *= 1000000ULL; break;
        case 'b': value *= 1000000000ULL; break;
        default: break;//return 0;//break; // no suffix
    }

    if (value < 0) value = 0;

    return (uint64_t)value;
}
*/

uint64_t parse_number_u64(const char *str);

static bool ParserConfig(Connection *c, const char *key, const char *value) {
	// gateway server 
	if (strcmp(key, "server.addr") == 0) {
		strncpy(c->gateway_server.addr, value, 16);
		return true;
	}
	
	if (strcmp(key, "server.port") == 0) {
		c->gateway_server.port = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}

	// cloudflare tcp tunnel
	if (strcmp(key, "tunnel.enabled") == 0) {
		c->tunnel_enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "tunnel.addr") == 0) {
		strncpy(c->tunnel_addr, value, 16);
		return true;
	}
	
	if (strcmp(key, "tunnel.port") == 0) {
		c->tunnel_port = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	// client version 
	if (strcmp(key, "client.version_major") == 0) {
		c->app.version_major = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.version_minor") == 0) {
		c->app.version_minor = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.version_patch") == 0) {
		c->app.version_patch = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "client.language_code") == 0) {
		c->app.language_code = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	// login 
	if (strcmp(key, "account.igg_id") == 0) {
		c->auth.igg_id = (uint64_t)strtoull(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "account.device_uuid") == 0) {
		strncpy(c->auth.device_uuid, value, sizeof(c->auth.device_uuid));
		return true;
	}
	
	if (strcmp(key, "account.access_key") == 0) {
		strcpy(c->auth.session, value);
		c->auth.session_len = (uint16_t)strlen(c->auth.session);
		return true;
	}
	
	// command 
	if (strcmp(key, "command.input") == 0) {
		c->bot.command_input = ParseCommandChannel(value);
		return true;
	}
	
	if (strcmp(key, "command.output") == 0) {
		c->bot.command_output = ParseCommandChannel(value);
		return true;
	}
	
	if (strcmp(key, "command.prefix") == 0) {
		if (value[0] != '\0') {
			c->bot.command_prefix = value[0];
		}
		return true;
	}
	
	
	if (strcmp(key, "alliance.auto_help") == 0) {
		c->alliance.auto_help = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "alliance.auto_open_gifts") == 0) {
		c->alliance.auto_open_gifts = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "alliance.request_own_help") == 0) {
		c->alliance.request_own_help = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.enabled") == 0) {
		c->protection.enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_always_on") == 0) {
		c->protection.shield_always_on = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_on_incoming_attack") == 0) {
		c->protection.shield_on_incoming_attack = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_on_incoming_scout") == 0) {
		c->protection.shield_on_incoming_scout = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.shield_priority") == 0) {
		return ParseShieldPriority(c, value);
	}
	
	if (strcmp(key, "admin.name") == 0) {
		strncpy(c->bot.admin_name, value, sizeof(c->bot.admin_name) - 1);
		c->bot.admin_name[sizeof(c->bot.admin_name) - 1] = '\0';
		return true;
	}
	
	// Cargo ship setting 
	if (strcmp(key, "cargo_ship.auto_trade") == 0) {
		if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0) {
			return false;
		}
		
		c->market.settings.auto_trade = (strcmp(value, "true") == 0);
		return true;
	}
	
	// Spend resources 
	if (strcmp(key, "cargo_ship.spend_food") == 0) {
		c->market.settings.spend_food = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_rock") == 0) {
		c->market.settings.spend_rock = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_wood") == 0) {
		c->market.settings.spend_wood = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_ore") == 0) {
		c->market.settings.spend_ore  = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.spend_gold") == 0) {
		c->market.settings.spend_gold  = (strcmp(value, "true") == 0);
		return true;
	}
	
	// Use resources from bag
	if (strcmp(key, "cargo_ship.use_bag_rss") == 0) {
		c->market.settings.use_bag_rss  = (strcmp(value, "true") == 0);
		return true;
	}
	
	
	// Reserved resources 
	if (strcmp(key, "cargo_ship.reserve_food") == 0) {
		c->market.reserve.food  = (uint32_t)parse_number_u64(value);
		// printf("cargo_ship.reserve_food: %lu\n", c->market.reserve.food);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_rock") == 0) {
		c->market.reserve.rock  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_wood") == 0) {
		c->market.reserve.wood  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_ore") == 0) {
		c->market.reserve.ore  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	if (strcmp(key, "cargo_ship.reserve_gold") == 0) {
		c->market.reserve.gold  = (uint32_t)parse_number_u64(value);
		return true;
	}
	
	/* Wave A: troop recall */
	if (strcmp(key, "protection.recall_on_incoming_attack") == 0) {
		c->protection.recall_on_incoming_attack = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.recall_on_incoming_scout") == 0) {
		c->protection.recall_on_incoming_scout = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "protection.recall_on_incoming_conflict") == 0) {
		c->protection.recall_on_incoming_conflict = (strcmp(value, "true") == 0);
		return true;
	}
	
	/* Wave A: Darknest rally join */
	if (strcmp(key, "rally.enabled") == 0) {
		c->darknest.auto_join = (strcmp(value, "true") == 0);
		return true;
	}
	
	/* Wave A: banking system */
	if (strcmp(key, "bank.enabled") == 0) {
		c->bank.enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "bank.send_food") == 0) { c->bank.send_food = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.send_rock") == 0) { c->bank.send_rock = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.send_wood") == 0) { c->bank.send_wood = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.send_ore")  == 0) { c->bank.send_ore  = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.send_gold") == 0) { c->bank.send_gold = (strcmp(value, "true") == 0); return true; }
	
	if (strcmp(key, "bank.reserve_food") == 0) { c->bank.reserve.food = (uint32_t)parse_number_u64(value); return true; }
	if (strcmp(key, "bank.reserve_rock") == 0) { c->bank.reserve.rock = (uint32_t)parse_number_u64(value); return true; }
	if (strcmp(key, "bank.reserve_wood") == 0) { c->bank.reserve.wood = (uint32_t)parse_number_u64(value); return true; }
	if (strcmp(key, "bank.reserve_ore")  == 0) { c->bank.reserve.ore  = (uint32_t)parse_number_u64(value); return true; }
	if (strcmp(key, "bank.reserve_gold") == 0) { c->bank.reserve.gold = (uint32_t)parse_number_u64(value); return true; }
	
	if (strcmp(key, "bank.max_delivery_distance") == 0) {
		c->bank.max_delivery_distance = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "bank.use_bag_rss")  == 0) { c->bank.use_bag_rss  = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.use_bag_food") == 0) { c->bank.use_bag_food = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.use_bag_rock") == 0) { c->bank.use_bag_rock = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.use_bag_wood") == 0) { c->bank.use_bag_wood = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.use_bag_ore")  == 0) { c->bank.use_bag_ore  = (strcmp(value, "true") == 0); return true; }
	if (strcmp(key, "bank.use_bag_gold") == 0) { c->bank.use_bag_gold = (strcmp(value, "true") == 0); return true; }
	
	/* Wave A: automatic troop training */
	if (strcmp(key, "train.enabled") == 0) {
		c->train.enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "train.kind") == 0) {
		c->train.kind = (uint8_t)strtoul(value, NULL, 10);
		if (c->train.kind > 3) c->train.kind = 0;
		return true;
	}
	
	if (strcmp(key, "train.tier") == 0) {
		c->train.tier = (uint8_t)strtoul(value, NULL, 10);
		if (c->train.tier > 4) c->train.tier = 0;
		return true;
	}
	
	if (strncmp(key, "train.target_", 13) == 0) {
		/* target_infantry / target_ranged / target_cavalry /
		 * target_siege — totals including already-trained troops. */
		const char *k = key + 13;
		int idx = -1;
		if (!strcmp(k, "infantry")) idx = 0;
		else if (!strcmp(k, "ranged")) idx = 1;
		else if (!strcmp(k, "cavalry")) idx = 2;
		else if (!strcmp(k, "siege")) idx = 3;
		if (idx >= 0) {
			c->train.target_total[idx] =
				(uint32_t)strtoul(value, NULL, 0);
			return true;
		}
		return false;
	}
	
	if (strcmp(key, "train.dismiss_above") == 0) {
		c->train.dismiss_above = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "train.dismiss_batch") == 0) {
		c->train.dismiss_batch = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "train.max_batch") == 0) {
		c->train.max_batch = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "train.rotate") == 0) {
		/* "infantry,ranged,cavalry,siege" — 0 means no rotation and
		 * the single configured kind is used, as before. */
		char buf[64];
		snprintf(buf, sizeof(buf), "%s", value);
		c->train.rotate_count = 0;
		for (char *tok = strtok(buf, ","); tok && c->train.rotate_count < 4;
		     tok = strtok(NULL, ",")) {
			uint8_t k = 4;
			if (!strcmp(tok, "infantry")) k = 0;
			else if (!strcmp(tok, "ranged")) k = 1;
			else if (!strcmp(tok, "cavalry")) k = 2;
			else if (!strcmp(tok, "siege")) k = 3;
			if (k < 4)
				c->train.rotate_kind[c->train.rotate_count++] = k;
		}
		return true;
	}
	
	if (strcmp(key, "train.amount") == 0) {
		c->train.amount = (uint32_t)strtoul(value, NULL, 10);
		if (c->train.amount == 0) c->train.amount = 1000;
		return true;
	}
	
	if (strcmp(key, "train.interval_s") == 0) {
		c->train.interval_s = (uint16_t)strtoul(value, NULL, 10);
		if (c->train.interval_s < 30) c->train.interval_s = 30;
		return true;
	}
	
	if (strcmp(key, "train.instant_finish") == 0) {
		c->train.instant_finish = (strcmp(value, "true") == 0);
		return true;
	}
	
	/* Wave A: automatic speed-ups (beta) */
	if (strcmp(key, "speedup.enabled") == 0) {
		c->speedup.enabled = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "speedup.daily_cap") == 0) {
		c->speedup.daily_cap = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	/* Wave B/C/D automations (waves.c) */
	if (strcmp(key, "wave.build_upgrade") == 0) {
		c->wave.build_upgrade = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.build_priority") == 0) {
		char buffer[512];
		strncpy(buffer, value, sizeof(buffer));
		buffer[sizeof(buffer) - 1] = '\0';
		
		int count = 0;
		char *token = strtok(buffer, ",");
		while (token && count < WAVE_PRIORITY_MAX) {
			while (*token == ' ') token++;
			c->wave.build_priority[count++] =
				(uint16_t)strtoul(token, NULL, 10);
			token = strtok(NULL, ",");
		}
		c->wave.build_priority_count = (uint8_t)count;
		return true;
	}
	
	if (strcmp(key, "wave.build_max_level") == 0) {
		c->wave.build_max_level = (uint8_t)strtoul(value, NULL, 10);
		if (c->wave.build_max_level > 25) c->wave.build_max_level = 25;
		return true;
	}
	
	if (strcmp(key, "wave.research_auto") == 0) {
		c->wave.research_auto = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.research_priority") == 0) {
		char buffer[512];
		strncpy(buffer, value, sizeof(buffer));
		buffer[sizeof(buffer) - 1] = '\0';
		
		int count = 0;
		char *token = strtok(buffer, ",");
		while (token && count < WAVE_PRIORITY_MAX) {
			while (*token == ' ') token++;
			c->wave.research_priority[count++] =
				(uint16_t)strtoul(token, NULL, 10);
			token = strtok(NULL, ",");
		}
		c->wave.research_priority_count = (uint8_t)count;
		return true;
	}
	
	if (strcmp(key, "wave.shelter_always") == 0) {
		c->wave.shelter_always = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.shelter_always") == 0) {
		c->wave.shelter_always = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.shelter_on_attack") == 0) {
		c->wave.shelter_on_attack = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.shelter_on_scout") == 0) {
		c->wave.shelter_on_scout = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.quest_claim") == 0) {
		c->wave.quest_claim = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.vip_collect") == 0) {
		c->wave.vip_collect = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strncmp(key, "wave.gather_hero", 17) == 0) {
		int idx = atoi(key + 17);
		if (idx >= 1 && idx <= 5)
			c->wave.gather_hero[idx - 1] = (uint16_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strncmp(key, "wave.arena_offense_hero", 23) == 0) {
		int idx = atoi(key + 23);
		if (idx >= 1 && idx <= 5)
			c->wave.arena_offense_hero[idx - 1] =
				(uint16_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.arena_challenge") == 0) {
		c->wave.arena_challenge = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.gather_auto") == 0) {
		c->wave.gather_auto = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.hunt_auto") == 0) {
		c->wave.hunt_auto = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.hunt_max_level") == 0) {
		c->wave.hunt_max_level = (uint8_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "wave.scan_interval_s") == 0) {
		c->wave.scan_interval_s = (uint16_t)strtoul(value, NULL, 10);
		if (c->wave.scan_interval_s < 60)
			c->wave.scan_interval_s = 60;
		return true;
	}
	
	if (strcmp(key, "wave.trap_build") == 0) {
		c->wave.trap_build = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.pet_train") == 0) {
		c->wave.pet_train = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.reward_claim") == 0) {
		c->wave.reward_claim = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.reward_mask") == 0) {
		c->wave.reward_mask = (uint8_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.guild_fest") == 0) {
		c->wave.guild_fest = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.guildfest_difficulty") == 0) {
		c->wave.guildfest_difficulty = (uint8_t)strtoul(value, NULL, 0);
		if (c->wave.guildfest_difficulty > 4)
			c->wave.guildfest_difficulty = 4;
		return true;
	}
	
	if (strcmp(key, "wave.labyrinth") == 0) {
		c->wave.labyrinth = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.request_build_info") == 0) {
		c->wave.request_build_info = (strcmp(value, "true") == 0);
		return true;
	}
	
	/* wave.request_role_info has been removed: 1004 is the LOGIN
	 * request (_MSG_LOGIN_REQUESTLOGIN), not a role-data request, and
	 * sending it post-login is a re-auth attempt that closes the
	 * session. ROLEINFO is pushed at login; see main.c. */
	if (strcmp(key, "wave.request_role_info") == 0)
		return true;
	
	if (strcmp(key, "wave.trap_repair") == 0) {
		c->wave.trap_repair = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.trap_repair_batch") == 0) {
		c->wave.trap_repair_batch = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.regather_cooldown_s") == 0) {
		c->wave.regather_cooldown_s = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.action_min_gap_ms") == 0) {
		c->wave.action_min_gap_ms = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.load_equip_inventory") == 0) {
		c->wave.load_equip_inventory = (strcmp(value, "true") == 0);
		return true;
	}
	if (strcmp(key, "wave.online_gift") == 0) {
		c->wave.online_gift = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.tycoon") == 0) {
		c->wave.tycoon = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.labyrinth_mode") == 0) {
		c->wave.labyrinth_mode = (uint8_t)strtoul(value, NULL, 0);
		if (c->wave.labyrinth_mode > 1)
			c->wave.labyrinth_mode = 1;
		return true;
	}
	
	if (strcmp(key, "wave.labyrinth_spend") == 0) {
		c->wave.labyrinth_spend = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.stage_sweep") == 0) {
		c->wave.stage_sweep = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.heal_troops") == 0) {
		c->wave.heal_troops = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.heal_style") == 0) {
		c->wave.heal_style = (uint8_t)strtoul(value, NULL, 10);
		if (c->wave.heal_style > 2)
			c->wave.heal_style = 0;
		return true;
	}
	
	if (strcmp(key, "wave.gather_interval_s") == 0) {
		c->wave.gather_interval_s = (uint16_t)strtoul(value, NULL, 10);
		if (c->wave.gather_interval_s < 30)
			c->wave.gather_interval_s = 30;
		return true;
	}
	
	if (strcmp(key, "wave.gather_min_amount") == 0) {
		c->wave.gather_min_amount = (uint32_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "wave.gather_max_dist") == 0) {
		c->wave.gather_max_dist = (uint16_t)strtoul(value, NULL, 10);
		return true;
	}
	
	if (strcmp(key, "wave.gather_gems_first") == 0) {
		c->wave.gather_gems_first = (strcmp(value, "true") == 0);
		return true;
	}
	if (strcmp(key, "wave.gather_gem_load") == 0) {
		c->wave.gather_gem_load = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	if (strcmp(key, "wave.gather_gem_max_dist") == 0) {
		c->wave.gather_gem_max_dist = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	if (strcmp(key, "wave.gather_gem_min_level") == 0) {
		c->wave.gather_gem_min_level = (uint8_t)strtoul(value, NULL, 0);
		return true;
	}
	if (strcmp(key, "wave.gather_stock_target") == 0) {
		c->wave.gather_stock_target = (uint32_t)strtoul(value, NULL, 0);
		return true;
	}
	
	if (strcmp(key, "wave.gather_priority") == 0) {
		/* amount | mixed | distance | lowest (or 0 | 1 | 2 | 3).
		 * "lowest" targets whichever selected resource the castle is
		 * shortest of, which beats amount-maximising on a growing
		 * account; see wave.gather_stock_target. */
		if (strcmp(value, "amount") == 0 || strcmp(value, "0") == 0)
			c->wave.gather_priority = 0;
		else if (strcmp(value, "distance") == 0 ||
		         strcmp(value, "2") == 0)
			c->wave.gather_priority = 2;
		else if (strcmp(value, "lowest") == 0 ||
		         strcmp(value, "3") == 0)
			c->wave.gather_priority = 3;
		else
			c->wave.gather_priority = 1;
		return true;
	}
	
	if (strcmp(key, "wave.gather_fit") == 0) {
		c->wave.gather_fit = (strcmp(value, "true") == 0);
		return true;
	}
	
	if (strcmp(key, "wave.gather_load") == 0) {
		c->wave.gather_load = (uint32_t)strtoul(value, NULL, 10);
		if (c->wave.gather_load == 0)
			c->wave.gather_load = 10;
		return true;
	}
	
	if (strcmp(key, "wave.hunt_min_level") == 0) {
		c->wave.hunt_min_level = (uint8_t)strtoul(value, NULL, 10);
		if (c->wave.hunt_min_level > 99)
			c->wave.hunt_min_level = 99;
		return true;
	}
	
	if (strcmp(key, "wave.log_packets") == 0) {
		c->wave.log_packets = (strcmp(value, "true") == 0);
		return true;
	}
	
	return true;
}

bool LoadConfig(Connection *c, const char *filename)
{
	FILE *fp = fopen(filename, "r");
	
	if (!fp) {
		return false;
	}
	
	char line[512];
	char key[64], value[512];
	uint32_t line_num = 0;
	
	while (fgets(line, sizeof(line), fp)) {
		line_num++;
		
		/* Skip blank lines */
		if (line[0] == '\n' || line[0] == '\r') 
			continue;
		
		/* Skip comments */
		if (line[0] == '#' || line[0] == ';' || (line[0] == '/' && line[1] == '/')) 
			continue;
		
		if (sscanf(line, " %63[^=]= %511[^\n]", key, value) != 2) {
			line[strcspn(line, "\r\n")] = '\0';
			printf("%s:%u: error: unexpected syntax: %s\n", filename, line_num, line);
			fclose(fp);
			return false;
		}
		
		key[strcspn(key, " \t")] = '\0';
		value[strcspn(value, "\r\n")] = '\0';	
		
		if (ParserConfig(c, key, value) != true) {
			printf("%s:%u: error: `%s`\n", filename, line_num, value);
			fclose(fp);
			return false;
		}
    }

    fclose(fp);
    return true;
}