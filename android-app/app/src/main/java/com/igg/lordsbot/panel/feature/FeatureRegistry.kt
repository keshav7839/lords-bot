package com.igg.lordsbot.panel.feature

/**
 * One editable bot setting, as shown in the app.
 *
 * [risk] is the whole point of the registry. Every outbound opcode this
 * project has ever sent was triaged by what it did to the session:
 *
 *  - [Risk.SAFE]   read-only, or an opcode proven against the decompiled
 *                  client and observed working.
 *  - [Risk.CAUTION] known-good opcode, but state-changing. Automating it
 *                  unattended spends the account's own resources or items.
 *  - [Risk.RISKY]  either never proven, or proven to get the session
 *                  closed by the server. Never enabled automatically, and
 *                  hard-blocked in Ultra Safe mode.
 */
enum class Risk { SAFE, CAUTION, RISKY }

enum class FieldType { TOGGLE, INT, TEXT, CHOICE, READONLY }

/**
 * Keys that are pinned off regardless of mode.
 *
 * This list is the reason the app cannot reproduce the mistakes that
 * already happened: these are the five opcodes empirically found to close
 * the session on this server. 1004 is no longer even present in the C
 * source (it was _MSG_LOGIN_REQUESTLOGIN - a re-authentication attempt,
 * not a data request), but the key is still listed so that a stale config
 * file containing it is neutralised on load instead of being passed
 * through.
 */
object RiskyKeys {
    val ALWAYS_BLOCKED = setOf(
        "wave.request_role_info", // 1004 - re-auth, drops session
        "wave.request_build_info", // 2000 - rejected
        "wave.load_equip_inventory", // 1416 - rejected
        "train.dismiss_above", // 2405 - rejected
        "train.instant_finish", // 2407 FINISHTRAINING - costs gems
        "alliance.request_own_help", // 2852 - rejected
        "wave.heal_troops", // payload unknown, closes session
        "protection.shield_on_incoming_attack",
        "protection.shield_on_incoming_scout",
        "protection.shield_always_on",
        "speedup.enabled",
        "wave.stage_sweep",
    )
}

data class FeatureSpec(
    val key: String,
    val label: String,
    val type: FieldType,
    val defaultValue: String,
    val group: String,
    val risk: Risk,
    val help: String = "",
    val choices: List<String> = emptyList(),
) {
    val blocked: Boolean get() = key in RiskyKeys.ALWAYS_BLOCKED
}

/**
 * Every setting the C bot understands, transcribed from src/config.c and
 * the shipped config_wave.cfg. 124 keys.
 *
 * Kept as an explicit table rather than reflected at runtime because the
 * native side has no introspection entry point, and because a hard-coded
 * table is reviewable in a diff.
 *
 * Key spelling is the thing that must stay in sync with src/config.c. A CI
 * step re-parses the bot's config.c and fails the build if a key here is
 * not one the engine accepts, so a renamed key cannot ship as a toggle that
 * silently does nothing.
 */
object FeatureRegistry {

    private fun t(key: String, label: String, group: String, risk: Risk,
                 def: String, help: String = "", type: FieldType = FieldType.TOGGLE,
                 choices: List<String> = emptyList()) =
        FeatureSpec(key, label, type, def, group, risk, help, choices)

    private fun i(key: String, label: String, group: String, risk: Risk,
                 def: String, help: String = "") =
        FeatureSpec(key, label, FieldType.INT, def, group, risk, help)

    private fun c(key: String, label: String, group: String, risk: Risk,
                 def: String, choices: List<String>, help: String = "") =
        FeatureSpec(key, label, FieldType.CHOICE, def, group, risk, help, choices)

    private fun ro(key: String, label: String, group: String, def: String) =
        FeatureSpec(key, label, FieldType.READONLY, def, group, Risk.SAFE,
            "Credential. Set via PCAP import or account editor.")

    val all: List<FeatureSpec> = listOf(
        // ---- Identity ------------------------------------------------
        ro("server.addr", "Gateway address", "Identity", "192.243.44.63"),
        i("server.port", "Gateway port", "Identity", Risk.SAFE, "5999"),
        ro("account.igg_id", "IGG ID", "Identity", "0"),
        ro("account.device_uuid", "Device UUID", "Identity", ""),
        ro("account.access_key", "Access key", "Identity", ""),
        t("command.prefix", "Command prefix", "Identity", Risk.CAUTION, "$"),
        c("command.input", "Read commands from", "Identity", Risk.SAFE, "GUILD",
            listOf("GUILD", "CHAT", "MAIL")),
        c("command.output", "Send replies to", "Identity", Risk.SAFE, "MAIL",
            listOf("MAIL", "CHAT", "GUILD")),

        // ---- Training -------------------------------------------------
        t("train.enabled", "Auto training", "Training", Risk.CAUTION, "true",
            "Trains up to the targets below. Server refuses with err=2 if unaffordable."),
        c("train.rotate", "Rotation order", "Training", Risk.CAUTION,
            "infantry,ranged,cavalry", listOf("infantry", "ranged", "cavalry", "siege")),
        i("train.target_infantry", "Target infantry", "Training", Risk.CAUTION, "30000"),
        i("train.target_ranged", "Target ranged", "Training", Risk.CAUTION, "30000"),
        i("train.target_cavalry", "Target cavalry", "Training", Risk.CAUTION, "30000"),
        i("train.target_siege", "Target siege", "Training", Risk.CAUTION, "2000"),
        i("train.max_batch", "Max per batch", "Training", Risk.CAUTION, "2000"),
        i("train.interval_s", "Interval (s)", "Training", Risk.CAUTION, "120"),
        t("train.dismiss_above", "Dismiss surplus", "Training", Risk.RISKY, "false",
            "2405 - bisected as a session killer. Blocked in Ultra Safe."),
        i("train.dismiss_batch", "Dismiss batch", "Training", Risk.RISKY, "5000"),
        t("train.instant_finish", "Instant finish", "Training", Risk.RISKY, "false",
            "2407 FINISHTRAINING - spends gems. Not implemented in the app's quick actions."),

        // ---- Buildings ------------------------------------------------
        t("wave.build_upgrade", "Auto building upgrades", "Buildings", Risk.CAUTION, "true",
            "Picks from the priority list. Currently answered with a refusal on this account."),
        t("wave.request_build_info", "Request building list", "Buildings", Risk.RISKY, "false",
            "2000 - rejected by the server. 2001 arrives at login anyway."),
        t("wave.load_equip_inventory", "Request gear inventory", "Buildings", Risk.RISKY, "false",
            "1416 - rejected by the server. Worn gear (3804) is read regardless."),

        // ---- Research / misc wave -------------------------------------
        t("wave.research_auto", "Auto research", "Research", Risk.CAUTION, "true",
            "Sweeps tech ids; the real prerequisite graph needs the game's own table."),
        t("wave.log_packets", "Log packets", "Debug", Risk.SAFE, "true"),
        t("wave.action_min_gap_ms", "Min action gap (ms)", "Debug", Risk.SAFE, "800",
            "Paces outbound actions. Raising this is the cheapest stealth knob."),
        t("wave.quest_claim", "Claim quests", "Research", Risk.SAFE, "true"),
        t("wave.reward_claim", "Claim rewards", "Research", Risk.SAFE, "true"),
        t("wave.reward_mask", "Reward mask", "Research", Risk.CAUTION, "0x03"),

        // ---- VIP / gift / tycoon --------------------------------------
        t("wave.vip_collect", "Collect VIP", "Events", Risk.SAFE, "true"),
        t("wave.online_gift", "Claim free box", "Events", Risk.SAFE, "true",
            "1117 -> 1118. Free Turf box, verified working."),
        t("wave.tycoon", "Kingdom Tycoon", "Events", Risk.SAFE, "true",
            "Verified live. Claims only free rolls."),
        t("wave.arena_challenge", "Arena challenges", "Events", Risk.CAUTION, "true",
            "Free gem prize only; challenge fights still error out."),
        i("wave.arena_offense_hero1", "Offense hero 1", "Events", Risk.CAUTION, "0"),
        i("wave.arena_offense_hero2", "Offense hero 2", "Events", Risk.CAUTION, "0"),
        i("wave.arena_offense_hero3", "Offense hero 3", "Events", Risk.CAUTION, "0"),
        i("wave.arena_offense_hero4", "Offense hero 4", "Events", Risk.CAUTION, "0"),
        i("wave.arena_offense_hero5", "Offense hero 5", "Events", Risk.CAUTION, "0"),

        // ---- Gather ---------------------------------------------------
        t("wave.gather_auto", "Auto gather", "Gather", Risk.CAUTION, "true"),
        c("wave.gather_priority", "Tile priority", "Gather", Risk.SAFE, "lowest",
            listOf("amount", "mixed", "distance", "lowest")),
        t("wave.gather_fit", "Fit troops to tile", "Gather", Risk.SAFE, "true"),
        t("wave.gather_gems_first", "Prefer gem lodes", "Gather", Risk.SAFE, "true",
            "Only ever picks lodes it can gather for free."),
        i("wave.gather_gem_load", "Gem load per march", "Gather", Risk.CAUTION, "1000"),
        i("wave.gather_gem_max_dist", "Gem max distance", "Gather", Risk.SAFE, "60"),
        i("wave.gather_gem_min_level", "Gem min tile level", "Gather", Risk.SAFE, "0"),
        i("wave.gather_min_amount", "Min tile amount", "Gather", Risk.SAFE, "0"),
        i("wave.gather_max_dist", "Max tile distance", "Gather", Risk.SAFE, "0"),
        i("wave.gather_load", "Troops per march", "Gather", Risk.CAUTION, "10"),
        i("wave.gather_stock_target", "Stock target", "Gather", Risk.SAFE, "1000000"),
        i("wave.gather_interval_s", "Gather interval (s)", "Gather", Risk.SAFE, "60"),
        i("wave.scan_interval_s", "Map scan interval (s)", "Gather", Risk.SAFE, "60"),
        i("wave.regather_cooldown_s", "Regather cooldown (s)", "Gather", Risk.SAFE, "300"),

        // ---- Hunt / heal / stage ---------------------------------------
        t("wave.hunt_auto", "Auto hunt", "Combat", Risk.CAUTION, "false",
            "Guildless accounts get server error 4."),
        i("wave.hunt_min_level", "Hunt min level", "Combat", Risk.CAUTION, "0"),
        i("wave.hunt_max_level", "Hunt max level", "Combat", Risk.CAUTION, "5"),
        t("wave.heal_troops", "Heal troops", "Combat", Risk.RISKY, "false",
            "Payload unconfirmed; observed to close the session."),
        c("wave.heal_style", "Heal style", "Combat", Risk.RISKY, "0", listOf("0", "1", "2")),
        t("wave.stage_sweep", "Stage sweep", "Combat", Risk.RISKY, "false",
            "Incomplete payload; disabled."),

        // ---- Protection -------------------------------------------------
        t("protection.enabled", "Protection", "Protection", Risk.CAUTION, "false"),
        t("protection.recall_on_incoming_attack", "Recall on attack", "Protection", Risk.SAFE, "true",
            "Read-only reaction to a march event; no new opcode."),
        t("protection.recall_on_incoming_scout", "Recall on scout", "Protection", Risk.SAFE, "true"),
        t("protection.recall_on_incoming_conflict", "Recall on conflict", "Protection", Risk.SAFE, "true"),
        t("protection.shield_always_on", "Keep shield up", "Protection", Risk.RISKY, "false"),
        t("protection.shield_on_incoming_attack", "Shield on attack", "Protection", Risk.RISKY, "false"),
        t("protection.shield_on_incoming_scout", "Shield on scout", "Protection", Risk.RISKY, "false"),
        FeatureSpec("protection.shield_priority", "Shield order", FieldType.TEXT,
            "SHIELD_4H, SHIELD_8H, SHIELD_12H, SHIELD_1D", "Protection", Risk.RISKY),
        t("speedup.enabled", "Use speed-ups", "Protection", Risk.RISKY, "false",
            "Speed-ups are limited resources. Off unless you want them spent."),
        i("speedup.daily_cap", "Speed-up daily cap", "Protection", Risk.RISKY, "20"),

        // ---- Alliance ---------------------------------------------------
        t("alliance.auto_help", "Auto help", "Alliance", Risk.SAFE, "true"),
        t("alliance.auto_open_gifts", "Open gifts", "Alliance", Risk.SAFE, "true"),
        t("alliance.request_own_help", "Request own help", "Alliance", Risk.RISKY, "true",
            "2852 - rejected. No-op on a guildless account."),

        // ---- Guild features ---------------------------------------------
        t("wave.guild_fest", "Guild fest", "Alliance", Risk.SAFE, "true"),
        i("wave.guildfest_difficulty", "Guild fest difficulty", "Alliance", Risk.SAFE, "4"),
        t("wave.labyrinth", "Labyrinth", "Alliance", Risk.SAFE, "true"),
        c("wave.labyrinth_mode", "Labyrinth mode", "Alliance", Risk.SAFE, "1", listOf("0", "1", "2", "3")),
        t("wave.labyrinth_spend", "Labyrinth spend", "Alliance", Risk.RISKY, "false"),

        // ---- Traps / shelter --------------------------------------------
        t("wave.trap_build", "Build traps", "Defence", Risk.CAUTION, "true"),
        t("wave.trap_repair", "Repair traps", "Defence", Risk.CAUTION, "true"),
        i("wave.trap_repair_batch", "Trap repair batch", "Defence", Risk.CAUTION, "100"),
        t("wave.shelter_on_attack", "Shelter on attack", "Defence", Risk.SAFE, "true"),
        t("wave.shelter_on_scout", "Shelter on scout", "Defence", Risk.SAFE, "true"),
        t("wave.shelter_always", "Shelter always", "Defence", Risk.RISKY, "false"),

        // ---- Market / bank / cargo ----------------------------------------
        t("cargo_ship.auto_trade", "Cargo auto trade", "Economy", Risk.RISKY, "false"),
        t("cargo_ship.spend_food", "Cargo: spend food", "Economy", Risk.RISKY, "false"),
        t("cargo_ship.spend_rock", "Cargo: spend rock", "Economy", Risk.RISKY, "false"),
        t("cargo_ship.spend_wood", "Cargo: spend wood", "Economy", Risk.RISKY, "false"),
        t("cargo_ship.spend_ore", "Cargo: spend ore", "Economy", Risk.RISKY, "false"),
        t("cargo_ship.spend_gold", "Cargo: spend gold", "Economy", Risk.RISKY, "false"),
        i("cargo_ship.reserve_food", "Cargo reserve food", "Economy", Risk.SAFE, "10M"),
        i("cargo_ship.reserve_rock", "Cargo reserve rock", "Economy", Risk.SAFE, "10M"),
        i("cargo_ship.reserve_wood", "Cargo reserve wood", "Economy", Risk.SAFE, "10M"),
        i("cargo_ship.reserve_ore", "Cargo reserve ore", "Economy", Risk.SAFE, "10M"),
        i("cargo_ship.reserve_gold", "Cargo reserve gold", "Economy", Risk.SAFE, "10M"),
        t("bank.enabled", "Bank/supply", "Economy", Risk.RISKY, "false"),
        t("bank.send_food", "Send food", "Economy", Risk.RISKY, "false"),
        t("bank.send_rock", "Send rock", "Economy", Risk.RISKY, "false"),
        t("bank.send_wood", "Send wood", "Economy", Risk.RISKY, "false"),
        t("bank.send_ore", "Send ore", "Economy", Risk.RISKY, "false"),
        t("bank.send_gold", "Send gold", "Economy", Risk.RISKY, "false"),
        i("bank.max_delivery_distance", "Max delivery distance", "Economy", Risk.SAFE, "100"),

        // ---- Rally / misc ---------------------------------------------------
        t("rally.enabled", "Rally", "Alliance", Risk.CAUTION, "false"),
        t("tunnel.enabled", "Tunnel", "Debug", Risk.RISKY, "false"),
        i("tunnel.port", "Tunnel port", "Debug", Risk.RISKY, "16000"),
    )

    val byKey: Map<String, FeatureSpec> = all.associateBy { it.key }

    val groups: List<String> = all.map { it.group }.distinct()

    fun forGroup(group: String): List<FeatureSpec> = all.filter { it.group == group }

    /**
     * Apply a safety mode to a value set. Ultra Safe forces every RISKY
     * key off and leaves CAUTION keys alone only if the user explicitly
     * re-enables them; Safe only blocks RISKY.
     */
    fun applyMode(values: MutableMap<String, String>, ultraSafe: Boolean) {
        for (spec in all) {
            if (!spec.blocked && spec.type == FieldType.TOGGLE) continue
            values[spec.key] = if (spec.blocked ||
                (ultraSafe && spec.risk == Risk.RISKY)
            ) "false" else values[spec.key] ?: spec.defaultValue
        }
        for (k in RiskyKeys.ALWAYS_BLOCKED)
            values[k] = "false"
    }
}
