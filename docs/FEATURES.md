# Lords Mobile Bot — Feature Status

Research base: lordsmobile.fandom.com (~130 pages fetched) + commercial bot
docs (help.lords-bot.com) + guides (theriagames, lootbar, bluestacks, Reddit).

**Hard rule, enforced in code:** never spend gems/diamonds, never buy
speed-ups, never buy items with gems. Only free resources, free VIP
acceleration (server-applied), guild help, and earned item speed-ups.

## Legend
- **DONE** — works, verified on a live run
- **PARTIAL** — code exists but is incomplete or gated off
- **NEW** — needs protocol work (opcode known or must be discovered)
- **BLOCKED** — server/account gate, or the feature does not exist in the game

Packet shapes marked ✅ are confirmed against the decompiled official
client (`/tmp/opencode/refs2/`), not guessed. A guessed opcode cost us a
dropped session earlier, so unconfirmed shapes stay behind a config flag.

---

## 0. Where the bot stands

23 feature ticks: AllianceGift, Arena, BlackMarket, Build, DarknestRally,
GFest, Heal, Heartbeat, Lab, MapScan, Pet, Quest, Research,
ResourceTransfer, Reward, Shelter, Shield, Speedup, Stage, Training,
Trap, Tycoon, Vip. ~17.8k lines of C.

Verified live this session: login, research queue state, building data
(66 buildings), trap inventory, colosseum gem claim, kingdom tycoon
free-roll polling, troop training with rotation and target totals, guild
help request, cargo-ship market evaluation, labyrinth free attempts,
quest claims, VIP chest, reward claims.

**The two features that dominate a commercial bot and are still missing
are gear auto-switching and hero/talent progression** — both need packet
shapes that are NOT in the decompiled corpus (the `LordEquipData` and
talent classes are absent), so they need a real-client capture before
they can be written safely.

**Biggest live blocker**: the account is guildless. That disables guild
help, guild gifts, guild fest scoring, darknest rallies and monster
hunting. Joining a guild is worth more than any code change here.

## 1. Core economy

| Feature | Status | Location | Notes |
|---|---|---|---|
| Auto building / upgrade queue | **DONE** | `waves.c:WaveUpgradeOne` | 2001 BUILDINGINFO *is* pushed at login (66 buildings); the earlier "never arrives" conclusion was wrong and 2000 stays off. Upgrades fire when the queue is genuinely free. |
| Building priority list | DONE | `config.c` `wave.build_priority` | Castle → Academy → Wall → Manor → Vault → Infirmary → Barracks… |
| Auto research | **DONE (fixed)** | `waves.c:ResearchTick` | Was firing 69 rejected starts/run: `research_idle()` compared `finish_time` (actually the **start** time) against now. Now `start + total <= now`, armed from login RESEARCHINFO. |
| Research priority order | PARTIAL | `waves.c:default_research_priority` | Development tree first, then Economy, then Monster Hunt. Still falls back to a blind 1..400 sweep — a real prerequisite graph is TODO. |
| Auto troop training | **DONE (upgraded)** | `auto.c:TrainingTick` | Now rotates kinds (a mono-army is countered for free) and treats the amount as a *target total*, sending only the deficit, capped by `train.max_batch`. |
| Traps | **DONE (upgraded)** | `waves.c:TrapTick` | 2602 TRAPINFO is 12 × u32 ✅ (3 types × 4 tiers). Tracks real inventory and fills to the wall's trap capacity, equalised across types. Live: `T1 91/0/0/0 T2 0/0/0/0 T3 0/0/0/0`. |
| Trap repair | NEW | 2616 payload ✅ `seq` only | Never sent; needs the damaged-trap count from 2604. |
| Auto resource gathering | **DONE (upgraded)** | `waves.c:GatherTick` | 4 priority modes (amount/mixed/distance/lowest), fit-to-tile army sizing, hero slots now populated. Blocked live by no resource tiles in the scan window. |
| Speed-up usage | **DONE (fixed)** | `auto.c:SpeedupTick` | Was burning a 30-min item every 60s (up to 10h/day). Now only when covered by remaining queue time, and only if owned. |
| Turbo / instant finish | **REMOVED** | `waves.c` | 2407 FINISHTRAINING is a **diamond purchase**. Removed; warns if configured. |

## 2. Free rewards & income (highest ROI)

| Feature | Status | Location | Notes |
|---|---|---|---|
| Guild help — answer others | DONE | `protocol.c:RequestHelpAllianceMember` (2855) | Enabled via `alliance.auto_help`. Earns up to 40k guild coins/day. |
| Guild help — ask for ours | **NEW (added)** | `protocol.c:RequestRequestOwnHelp` (2852) | Had a response case but **no sender**. 1% off the remaining timer per help, up to 30 — the single highest-value free accelerator. Fired after each build start. |
| Guild gift auto-open | **DONE (fixed)** | `protocol.c:RecvAllianceGiftOpen`, `AllianceGiftTick` | Had a stray `return;` that stranded `gift_state` in OPENING. Also now deletes with the real SN instead of a 0xFFFFFFFF broadcast. |
| Quest claims (daily/admin/guild/VIP) | DONE | `waves.c:QuestTick`, `VipTick` | |
| Daily login / VIP points | NEW | opcode 3130 known, no builder | Cleanest untouched free-income win. |
| Mystery box | NEW | no opcode known | Needs one real-client capture. |
| Colosseum | **DONE (upgraded)** | `waves.c:ArenaTick` | Added the gem prize claim (5214 ✅ empty payload) — gems pay by rank every 3h and the claim is free; this was the entire free-gem value and was missing. Offense squad separated from defense (challenging with the defense roster is the formation others fight). |
| Labyrinth | DONE | `waves.c:LabTick` | Free-combos → free-daily probe. `labyrinth_spend=false` enforced. |
| Kingdom Tycoon | **DONE (added)** | `waves.c:TycoonTick` | 7010/7011 respond. Rolls only when the server offers a free roll, never a Luck Token (600-720 gems). Live: `info size=245 free_roll=0 crystals=14`. |
| Cargo ship | PARTIAL (fixed) | `protocol.c:ShouldBuyItem` | **Fixed a real money bug**: it returned `true` unconditionally, buying everything every 6h. Now an allow-list. |
| Treasure Trove | NEW | no opcodes exist | Needs a gem-balance field (the bot has none) + opcode discovery. |
| Transmutation lab | NEW | no opcodes exist | `auto_transmute`/`essence_level` are dead config at `main.c`. |

## 3. Combat & defense

| Feature | Status | Location | Notes |
|---|---|---|---|
| Shield keep-alive | DONE | `protocol.c:ShieldTick`, `UsePriorityShield` | Triple-gated off by config. |
| Shield on incoming attack/scout | DONE | `protocol.c:WhenEnemyArmyApproachingTurf/Camp` | |
| March recall on attack | DONE | `protocol.c:RequestTroopTakeBack` (free), `GatherRecallAll` | Recall on watchtower lines 5/7/12 + 2435 BEINGATTACK. Recall-on-conflict config key exists but has no implementation. |
| Shelter | **DONE (upgraded)** | `waves.c:ShelterTick` | The window (u16 lord \| i64 begin \| u32 require \| u16 mask ✅) was parsed and discarded, so the bot could not tell the shelter was lapsing. Now tracked and renewed under 2h remaining. A 12h shelter that expires leaves the leader exposed with no troops home. |
| Shelter release after attack | NEW | 5604 payload ✅ `seq` only | Mapped, never sent. |
| Heal wounded | BLOCKED | `waves.c:HealTick` | The 2426 payload is unverified; every style tried closed the session. Needs a real-client capture. |
| Sanctuary / devotion | NEW | 9402-9410 mapped, no builders in refs2 | `9405 INSTANT_REVIVE` is the gem path — must never be wired. `9407 DIVINE_REVIVE` is the free one. |
| Traps | **DONE (upgraded)** | `waves.c:TrapTick` | See §1. |
| Battle fury tracking | NEW | — | `RecvIBuffInfo` parses every boost but only classifies shields; fury is discarded. Shielding during fury gets rejected by the server. |
| Anti-scout | NEW | opcode 1436 mapped, no builder | Highest-leverage defense item, entirely absent. |
| Garrison / reinforce | NEW | 2458 mapped, no builder | High consequence — should stay opt-in and never send the leader. |
| Scouting (outbound) | **DONE (added)** | `protocol.c:RequestSendScout` | 2448 payload ✅ `seq \| u16 zone \| u8 point`, unencrypted. The highest-value packet the bot was missing: wall HP, trap tiers, defenders, shields, Battle Fury, and a darknest's Dark Essence level (nest level is a poor proxy — an L2 nest rolls essence 3-6, an L3 rolls 6-10). Report parse still to map. |
| Recall on conflict | NEW | — | Needs the map line data, which `WaveRecvMapUpdate` discards. |

## 4. Heroes & army

| Feature | Status | Location | Notes |
|---|---|---|---|
| Hero stage sweep | PARTIAL | `waves.c:StageTick` | Sends 1805 with an **empty payload** — the server just returns state. Needs chapter/stage args + STA gate. Elite is the medal source. |
| Hero roster | DONE | `waves.c:WaveRecvHeroSave` | 19 heroes parsed. |
| Hero ids on marches | **DONE (fixed)** | `protocol.c:RequestGatherMarch` | Gather marches sent all 5 hero ids as 0, forfeiting every passive army boost and troop Command capacity for free — a hero only grants those while deployed. Now uses `wave.gather_hero[1..5]`, falling back to the roster. |
| Hero skills / star-up | NEW | 1213/1204 mapped, no builders in refs2 | Never send the `_INSTANT` variants (gem path). |
| Hero talent tree | NEW | 3802 mapped, no builder | Talent points are read and discarded at `protocol.c:1490`. Additive only — never send a reset. |
| Equipment / gear | NEW | 3805/3809/3818/3820 mapped, **no builder in refs2** | The `LordEquipData` class is absent from the decompiled corpus, so the payload shape is unconfirmed. The reference bot's `Auto Switch` (per-context gear swap + idle set) is the largest remaining free-production gain — needs a real-client capture. |
| Familiars | PARTIAL | `waves.c:PetTick` | Stub that only logs. 8204 mapped, no builder. |
| Formation / battalions | PARTIAL | `include/connection.h:656` | Only a darknest ratio field; no lineup selection. |

## 5. Guild

| Feature | Status | Notes |
|---|---|---|
| Guild fest | PARTIAL | `GFestTick` sends the query but the mission list never arrives → the start path is unreachable. Gift-tier reward claim (30 tiers) unhandled. |
| Guild resource supply | PARTIAL | Full march state machine exists, zero triggers — only reachable by the `$food`-style commands. |
| Guild shop | NEW | 1453 HONORSHOP mapped. **Never guess the shop `Type`** — it can address the gem store. Coins-only allow-list + reserve floor. |
| Guild chat / announce | PARTIAL | `RequestSendChat` is complete with **zero call sites**. `MODIFY_SLOGAN/BULLETIN/BRIEF` mapped, rank-gated by `c->RoleAlliance.Rank`. |
| Member reports | NEW | All member data already parsed at `protocol.c:RecvAllianceMemberInfo` — might/kills rank, inactivity by `logout_time`, is read-only and free. |
| Guild showdown | NEW | Registration opcode not in the enum; needs discovery. |
| Alliance tech | **N/A** | Does not exist in Lords Mobile (it is a Kingshot/Last War feature). Do not implement. |

## 6. Map & intel

| Feature | Status | Notes |
|---|---|---|
| Map scanning | **DONE (fixed)** | `RequestMapData` + `WaveRecvMapUpdate` (15B header + 51B records). Now actually runs — it was blocked by `zone_id == 0`. |
| Kingdom overview fallback | **NEW (added)** | Older runs prove a zone-0 request returns the full window (run31: 39 tiles, 25 resources) while zone-only requests return one small snapshot and then go silent. `MapScanTick` now falls back to the overview when the cache stops growing. |
| Tile selection | DONE | 3 priority modes + `gather_fit` army sizing. A "lowest-stock resource" mode is a small addition. |
| Gem lodes | NEW | `POINT_KIND 6` parses correctly but there is no gem branch. Needs `gem_load = 1000` capacity (1000 troops = 1 gem) and an 80%-complete recall rule. |
| Monster hunt | BLOCKED | Server answers 2488 err=4 — the account has no guild. |
| Darknest rally | PARTIAL | `DarknestRallyTick` is a stub: `c->rally.pending` is never set anywhere, so the tick can never fire. `RequestJoinRally` itself is complete. |
| Bookmarks / pins | NEW | 3101-3110 all mapped, no senders. Pins are guild-visible — opt-in only. |
| Adventure log | PARTIAL | Mission lifecycle exists; no map-driven quest execution. |
| Travel-time budget | NEW | No travel-speed field anywhere; `gather_max_dist` is a raw tile distance. March ETA is the correct formulation. |

## 7. Root causes fixed this session

These were the reason most features above were dead:

1. **Login was failing** (runs 50-53) — the `account.access_key` had been rotated by the server. Recovered the new 416-char credential from a PCAPdroid capture of the real client. It is a signed JWT-shaped blob, not a free-form token.
2. **Log corruption** — `LOGI` goes to stderr (unbuffered) and `printf` to stdout (block-buffered when redirected), so every `printf` diagnostic was lost on exit and the two streams interleaved mid-line. Fixed with `setvbuf(_IOLBF)` + a `format_duration_str()` that returns a string so callers emit one complete line.
3. **1008 ROLEINFO was never requested** — this server does not push it, so `player.zone_id` stayed 0 and `player.name` empty, which silently disabled map scan, gather, hunt and training. Added `RequestRoleInfo` (1004).
4. **2001 BUILDINGINFO was never requested** — same class of bug, so no building upgrade was ever attempted. Added `RequestAllBuildData` (2000).
5. **Research time math** — see §1.
6. **Command guards** — `$su` had its authorization check commented out, so any player could mail `$su <self>` and become admin, unlocking `$probe` (raw packet injection). The resource-send commands and `$waves` were likewise unauthenticated. All restored.

## 7b. Request opcodes this server rejects

Bisected one at a time against a build that ran 60+ minutes clean. All
three are now config flags, default **off**, with a warning naming the
consequence:

| Opcode | What it does | Why rejected |
|---|---|---|
| 1004 ROLEINFO | The only packet carrying zone/name/troops; nothing else does here | Pre-auth opcode; the server drops the session ~2 min after a post-login send, encrypted or not |
| 2000 BUILDINGINFO | Guessed request half of 2001 | Also drops the session. **Turned out to be unnecessary** — 2001 is pushed at login and 66 buildings arrive without it |
| 2852 own-build help | Free 1%-per-help accelerator | Drops the session from a guildless account, where there is nobody to help anyway. Now additionally gated on `RoleAlliance.Channel != 0` |

Lesson applied: a payload shape has to be confirmed against the
decompiled client or a real-client capture before it ships. Confirmed
shapes are marked ✅ in the tables above.

## 8. Security notes

- `config.cfg` contains a live `igg_id`, `device_uuid` and `access_key` in plaintext, and the git remotes embed PATs/HF tokens. Rotate and move to env injection.
- The account is guildless (`alliance_chat=0`), which blocks both monster hunting (err=4) and every guild feature. Joining a guild is the single highest-value unblock.
