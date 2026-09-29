# Lords Mobile Bot — Complete Feature Inventory (web-fetched)

Sources walked exhaustively, 2026-09-29:
- **help.lords-bot.com** — 59 KB posts + 10 static pages + 5 topic indexes (the de-facto reference implementation)
- **GitHub** — garvitjain94/lords_mobile, halloweeks/lords-mobile-bot, denis1233-cloud/lords-mobile-bot (BotIgg), cupid25/LordsBot, effekt/LordsMobilePython, effekt/LordsMobileCSharp, jmatg1/lords-mobile-bot, fatrolls/Lords-mobile-bot, dragonDadem/LordsMobileBot, supdams/Lords-Mobile-Bot-SupDams (decompiled game source), + 12 more
- **Commercial/hosted** — gamebots.run, BotSauce, GnBots
- **Intel/tracking** — lordsmobilecartograph.ru (full), lordsrally.com (79 commands), blazebots.co.in
- **Guides** — lordsmobile.org, theriagames, lootbar, blueStacks, Reddit r/lordsmobile (.rss endpoint)

Legend for status vs **our** bot: ✅ DONE · 🟡 PARTIAL · 🔵 NEW (opcode known) · ⛔ UNKNOWN shape (needs capture) · 🚫 BLOCKED (server/account) · ❌ does not exist in game

---

## 1. What the reference bot exposes (toggle-for-toggle)

### General
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Use VIP Points (bag items) | ✅ | 🔵 | free items only |
| Open All Chests | ✅ | 🔵 | free items |
| Use Resources from Bag (incl. Anima/Lunite) | ✅ | 🔵 | "smart algorithm" — deficit cover then one overshoot |
| Use Star Scrolls (100%-success only) | ✅ | ⛔ | |
| Use Exp / Exp-Boost Items | ✅ | 🔵 | |
| Put Gems in Treasure Trove (30d + auto-collect) | ✅ | 🔵 | needs gem-balance field |
| **Mystery Box** (~15 min) | ✅ | 🔵 | **proto 1117/1118 known from BotIgg** |
| Admin/Guild Quests (VIP-aware batch) | ✅ | 🟡 | we do one-by-one |
| VIP Quests/Chests | ✅ | ✅ | |
| Turf Quests | ✅ | ✅ | |
| Daily Login Gift (21-day) | ✅ | 🔵 | protos 3605 + 3130 |
| Open all Admin/Guild Quests + Quest Reserve | ✅ | 🔵 | |
| Adventure Log | ✅ | 🔵 | 9713/9715 mapped |
| Send Help + cooldown | ✅ | ✅ | |
| **Request Help (own builds)** | ✅ | ✅ | added this session |
| Collect Guild Gifts (+ auto-delete) | ✅ | ✅ | fixed this session |
| Join Guild Showdown | ✅ | 🔵 | needs registration opcode (not in enum) |
| **Slow Help Speed when bar full** | ✅ | 🔵 | *courtesy rule* — let others fill their bars |
| Collect Fortune Packets | ✅ | 🔵 | |
| **Scheduled Build Spam** (N × build→help→cancel) | ✅ | 🔵 | needs cancel-build opcode |
| **Wait Until Help is Full** (before speed-ups) | ✅ | 🟡 | partial |
| **Smarts Speed-ups** (never waste a partial item) | ✅ | ✅ | fixed this session |
| Turf Boots (7d before 24h, never at max capacity) | ✅ | 🔵 | |
| Lottery (free mode only) | ✅ | 🔵 | |
| Worker Speed (1s between actions) | ✅ | 🔵 | global pacing |

### Protection
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Always shielded + redeploy threshold | ✅ | 🟡 | off by config |
| Use longer shields first (default = shortest) | ✅ | 🟡 | |
| Shield when under attack (~2–4s from march) | ✅ | ✅ | |
| Shield when rallied | ✅ | 🟡 | |
| **Shield when scouted** (fires at march-walk, not landing) | ✅ | ✅ | |
| **Always Anti-Scout** / on scouted / longer-first / redeploy | ✅ | 🔵 | opcode 1436, no builder in refs2 |
| **Prevent Shielding while prisoners held** | ✅ | ❌ | prison page cross-override |
| Recall troops if attacked | ✅ | ✅ | |
| **Recall troops if scouted** | ✅ | ✅ | |
| **Recall on conflict** (Withdraw Squad; skip while shielded) | ✅ | 🔵 | config key exists, no impl |
| **Time to wait before regathering** | ✅ | 🔵 | |
| Auto Recall Camps (tile vanished mid-march) | ✅ | 🔵 | |
| Don't Shelter / Always Shelter (12h, re-shelter ~2h) | ✅ | ✅ | renewal added this session |
| Recall Sheltered Troops After Attack | ✅ | 🔵 | 5604 payload confirmed (`seq` only) |
| Dont Shelter Siege | ✅ | 🔵 | |
| Shelter Type (hero+1 / hero+best) | ✅ | 🔵 | |
| **Battle Fury** (blocks shields) | 🟡 | 🔵 | must gate shielding on it |

### Buildings / Research
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Auto Build | ✅ | ✅ | |
| **Lowest Level First** | ✅ | 🔵 | |
| Building Priority (Castle→Resource→Academy→Manor→Barracks/Infirmary→Monsterhold→Familiars→Trading Post→Resource-no-Manor→Treasure Trove→Workshop) | ✅ | ✅ | ours matches |
| Max Building Level | ✅ | ✅ | |
| **Strict** (stop upgrading unnecessary buildings) | ✅ | 🔵 | changelog 5.7 |
| Turf building grid / demolish | ✅ | 🔵 | |
| Auto Research | ✅ | ✅ | fixed this session |
| **Use Target System** (target + auto-prerequisites) | ✅ | 🟡 | ours is a blind 1..400 sweep |
| **Use Technolabes** (free instant-finish, **forfeits event points**) | ✅ | 🔵 | |
| Minimum Research Might for Technolabes | ✅ | 🔵 | |
| Research tree drag-ordering | ✅ | 🟡 | |

### Army
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Train Troops (targets = totals incl. trained) | ✅ | ✅ | added this session |
| **Rotate Troops** | ✅ | ✅ | added this session |
| Heal Troops (infirmary) | ✅ | 🚫 | 2426 payload kills the session |
| **Heal Sanctuary** (25% chunks) | ✅ | 🔵 | 9407 free path |
| Select chapter (per-skirmish training) | ✅ | 🔵 | |
| Attack skirmish when troops at N% | ✅ | 🔵 | 1832 quickpass |
| **Recall troops for skirmish** (pull from shelter/gather) | ✅ | 🔵 | |
| Attack Trial By Fire | ✅ | 🔵 | |
| Troop dismiss | ✅ | 🔵 | **payload confirmed** `seq\|u8 kind\|u8 tier-1\|u32 qty` |
| Stationed heroes / familiars / leader / avatar | ✅ | ❌ | manual in game |

### Gathering
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Gather Resources | ✅ | ✅ | |
| **Leave One Spare Army** | ✅ | 🔵 | |
| Max Armies To Gather | ✅ | 🟡 | |
| Gathering Levels 1–5 filter | ✅ | 🔵 | |
| Gathering Types (incl. **Gems/Lodes**) | ✅ | 🔵 | |
| **Gather resource with the lowest amount** | ✅ | ✅ | added this session (mode 3) |
| **Ignore level settings for gems** | ✅ | 🔵 | lodes = 1000 troops per gem |
| **Only gather clearable tiles** | ✅ | 🔵 | |
| Auto Recall Camps | ✅ | 🔵 | |
| **Use Gathering Gear** (blocks other gear swaps until landed) | ✅ | ⛔ | gear protocol unknown |
| **Max Travel Time** (not distance) | ✅ | 🔵 | no travel-speed field |
| Sending Delay | ✅ | 🔵 | |
| Search Multiplier (×360 tiles/screen) | ✅ | 🔵 | MAP_TILE_MAX is 200 |
| Minimum tile count (not applied to lodes) | ✅ | 🟡 | |
| Target highest level / closest / lowest resource | ✅ | ✅ | 4 modes now |
| Gathering Schedule (no day rollover) | ✅ | 🔵 | |
| Realm gathering (`ron`/`roff`) | ✅ | ❌ | separate tile class |

### Hunting / Rally
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Hunt Monsters | ✅ | 🚫 | server err=4: **account is guildless** |
| **Start Hunt When Energy > X% of total** | ✅ | ❌ | **no energy field anywhere in the bot** |
| Hunt Priority: Any / Full HP / Lowest HP / **Steal (<20% HP → x2)** | ✅ | 🔵 | |
| **Combo Prediction** (hunt once, re-hunt with ×N) | ✅ | 🔵 | |
| AGI / INT lineups or auto-select | ✅ | 🟡 | hunt march does send heroes |
| Use Winged Boots on steal races | ✅ | 🔵 | |
| Send unfinished monster coords to guild chat | ✅ | 🔵 | `RequestSendChat` exists, 0 call sites |
| **Join Rallies (Darknest only)** + level/travel/max-start filters | ✅ | 🟡 | `DarknestRallyTick` stub: `rally.pending` never set |
| Dont Join if Lab Full / Dont Fill / Dont Send Siege / Dont Send T5 | ✅ | 🔵 | |
| **Send One Type** (highest-ratio only) / **Add Buffers** (1k lowest tier) | ✅ | 🔵 | |
| **Transmute Dark Essences** | ✅ | 🔵 | no lab opcodes exist |
| Delete Essences below level | ✅ | 🚫 | costs gold |

### Heroes
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Hire New Heroes (~10 medals) | ✅ | 🔵 | |
| **Upgrade Heroes** (grade via medals) | ✅ | 🔵 | 1213, no builder |
| **Enhance Heroes** (equip items, rank to 8) | ✅ | 🔵 | 1204 |
| Use Hero EXP items (highest first) | ✅ | 🔵 | |
| Revive Dead Leader | ✅ | 🔵 | Revival Fruit only, never buy |
| **Auto Attack Hero Stages** (sequential, revert on fail) | ✅ | 🟡 | 1805 sent with **empty payload** |
| Sweep / 10× Sweep (≥100 stamina) / Braveheart when STA out | ✅ | 🟡 | |
| **Use Priority Mode** (get heroes to gold, ordered fallthrough) | ✅ | 🔵 | |
| **Colosseum: Collect Arena Gems** | ✅ | ✅ | added this session |
| Colosseum win-chance band (1000 simulated battles) | ✅ | 🔵 | |
| Colosseum offense vs defense squads | ✅ | ✅ | separated this session |
| Buy extra attempts | ✅ | 🚫 | 250 gems — **never** |
| Attack Guild Members | ✅ | 🚫 | socially hostile |

### Equipment / Familiars / Talents
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| **Auto Switch** (gather/train/build/research/familiar/lunar) | ✅ | ⛔ | `LordEquipData` absent from refs2 |
| **Idle Gear Set** + idle timer | ✅ | ⛔ | biggest free-production gain |
| Auto Craft / Auto Upgrade (ingredient-gated) | ✅ | ⛔ | |
| Use Cabinet Expanders (at max capacity) | ✅ | 🔵 | never buy with gems |
| Open Pacts / Merge Pacts | ✅ | 🟡 | `PetTick` is a stub |
| Train / Train Skills / Shatter Extra Runes / Upgrade Skill / Enhance | ✅ | 🔵 | 8204, no builder |
| **Distribute Heroes Evenly** across gym | ✅ | 🔵 | |
| Daily Fragment Limit | ✅ | 🔵 | |
| **Automatically Configure Talents** | ✅ | 🔵 | 3802, no builder |
| Max Talent per node / Talent points left | ✅ | 🔵 | points discarded at `protocol.c:1490` |
| Reset Talent Points | ✅ | 🚫 | costs gems/coins |

### Guild / Bank
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Guild info / member list / rank | ✅ | 🟡 | members already parsed |
| Join / Leave / Create guild | ✅ | 🔵 | 2813/2849 |
| **Manage Guild** (accept/kick/whitelist/blacklist/auto-rank) | ✅ | 🔵 | rank ladder R1/R2 none, R3→R1/R2, R4→R1-3, R5 any |
| Edit slogan/bulletin/announcement | ✅ | 🔵 | 2831/2843/2845, gate on `RoleAlliance.Rank` |
| **Guild Bank commands** (~60 commands) | ✅ | 🟡 | we have `$food`/`$bank bal` only |
| Supply: min → reserve drip, from bag, max travel, randomise speed | ✅ | 🔵 | state machine exists, 0 triggers |
| Auto Configure (min = vault+cap, reserve = 40% vault) | ✅ | 🔵 | |
| Guild Fest: min/max **point-range filter** | ✅ | 🟡 | list never arrives |
| Guild Fest: reward priority **Gems → MH Chest → random** | ✅ | 🔵 | |
| Guild Fest: Remove Missions / Duplicates (R4+) | ✅ | 🔵 | |
| Guild Fest: Send-Mail-to-Player for non-automatable | ✅ | 🔵 | |
| Buy Extra Mission | ✅ | 🚫 | 1000 gems |
| Save Guild List / Guild Fest Stats / Export | ✅ | 🔵 | read-only, free |
| Guild Showdown (auto team, always leader, recall from shelter) | ✅ | 🔵 | registration opcode not in enum |
| **Guild Coins: Boost Items + Reserve Guild Coins** | ✅ | 🔵 | 1453, must hard-pin shop type |
| Buy VIP Points | ✅ | 🚫 | gems |

### Resources / Special
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| **Supply Queue tab** (outstanding marches) | ✅ | 🔵 | |
| Relocate (Relocator/Random/Migration/Novice) | ✅ | 🔵 | 1003/1004/1275 in items.h |
| Migration cost calculator | ✅ | 🔵 | |
| Recall all armies / **Force Recall** (needs Recall Squad) | ✅ | 🟡 | |
| Bag: open all, use resources, boosts | ✅ | 🔵 | |
| **Cargo Ship** (resource-only, min stars, deny-list, reserves) | ✅ | 🟡 | buy-everything bug fixed |
| **Trove deposit 30d + auto-collect** | ✅ | 🔵 | |
| Artifacts: appraise, **free chests only**, weekly challenge, **check every ~8h** | ✅ | 🔵 | 9778/9794 |
| Labyrinth (free attempt only) | ✅ | ✅ | |
| **Kingdom Tycoon** (free attempt only) | ✅ | ✅ | added this session |
| Vergeway / Mystic Spire / Trial By Fire | ✅ | 🔵 | Vergeway = real-time, hard |
| **Use Gems master switch** | ✅ | ✅ | we hard-block instead |

### Schedule / Infra
| Feature | Ref bot | Ours | Notes |
|---|---|---|---|
| Schedule 1 (fixed) / 2 (N hours after reconnect) | ✅ | 🔵 | |
| Recall armies before going offline | ✅ | 🔵 | |
| **Monitor Shield / Anti-Scout / Shelter offline (re-logins to renew)** | ✅ | 🔵 | |
| Auto Reconnect / Other Login Reconnect Time | ✅ | 🔵 | we have none |
| **Worker Speed** (≥1s between actions) | ✅ | 🔵 | |
| Periodic Reset (brief offline refresh) | ✅ | 🔵 | |
| Save logs / clear after N days | ✅ | 🔵 | |
| **Proxies** (SOCKS5 IPv4, `IP:Port:User:Pw`) | ✅ | 🔵 | tunnel stub exists |
| Chat bot (Discord/Telegram) | ✅ | 🔵 | we have webcmd.txt |
| Bulk rename (random/ascending/internet names) | ✅ | 🔵 | |
| Bulk relocate (hive grid, coord list) | ✅ | 🔵 | |
| Prison: execute prisoners, ransom, **prevent shielding** | ✅ | 🔵 | 2874 dismiss = 2000 gems, never |
| Baron tracker (Royal Battleground only) | ✅ | 🔵 | |

---

## 2. Features unique to the protocol bots (not in the reference)

| Feature | Source | Ours |
|---|---|---|
| **War mode**: parse attacker troops from 2446, compute tier-weighted power (T1/T2=0, T3=0.3, T4=1, T5=1.5), require ≥250k, apply the **counter-formation** via 6801, switch outfit 6102, revert on arrival | BotIgg | 🔵 — the standout defensive feature no vendor has |
| War/tower/camp monitoring + rally re-sort by shortest remaining | BotIgg | 🟡 watchtower only |
| Training subsidy research maths (tech 95-98) | BotIgg | 🔵 |
| Bag-resource deficit algorithm (sort asc → exact cover → 1 overshoot) | BotIgg | 🔵 |
| Simulated hourly resource production with per-resource caps | BotIgg | 🔵 |
| `!findtile` / `!findmonster` / `!findnest` search commands (~70 tile radius) | Blaze / LB / LR | 🔵 |
| Ghost-rally rejection (max rally start time) | all | 🔵 |
| **Slow Help Speed when bar full** | LB | 🔵 |

---

## 3. The intel layer (tracking bots — a whole category we have none of)

lordsmobilecartograph.ru + lordsrally.com (79 commands):

**Passive alerts**: shield drop · burning/smoking relocation · nickname change · guild change · lord return · rally started · new castle in kingdom · castle left kingdom · special monster spawn · **fury start** · lord lost/jailed

**Analysis**: `/a` castle card (might, kills, shield, garrison, jail, 36-action history, hour-of-day activity histogram) · `/es` saved equipment sets with bonus calc · `/panel` live gear · `/z` fury castles · `/m` unshielded by might · `/adv` multi-filter (might, last-burn, last-activity, shield, camps, leader-free) · `/hm` kingdom heat map · `/getbots` expose shield-monitor bots · `/nch` name-change history

**Fingerprinting insight**: trackers watch the map for skin changes and marches, then the profile for **gear swaps**, then stats, then build an activity histogram. So *a continuously active account is indistinguishable from an always-active player*.

---

## 4. Community doctrine (this is the part that changes design)

**Ban profile** (3 years of observed bans, r/lordsmobile): every ban seen was on a **zero-spend** account. Accounts with purchase history get routed to a bot-detection team and come back. Guild leaders are suspended, not banned. Accounts that log in briefly and send RSS manually get flagged and restored on appeal.

**Fingerprints to avoid**:
1. Sequential/random numeric names (`ID.12345`, `jjekg13962`) → randomise names
2. Never dropping a shield → shields must lapse sometimes
3. Long idle gaps → "idle" should be low-frequency, never zero
4. Constant identical actions → jitter delays (one OSS bot uses ±20% delay, 5%/30-120s natural breaks)
5. **Do not run full-play war automation** — "reining and warring is basically why we play"

**Accepted / green-lit**: resource supply to a bank, gem collection, economy chores. *"RSS bots: they're great — they make the game cheaper and less stressful."*

**The counter-intelligence tip** worth acting on: *"I never shielded my main… Anti-scout and a basic bot will keep the trackers off you because they won't know if you're trapping or just sitting since your bot is always doing something."*

**Gear ordering** (the single most important cross-cutting rule): gathering gear → marches → **keep it until troops land** → swap to research gear the moment they start collecting → war gear only in actual battle, never while travelling. Gear changes are *events with a completion condition*, not instants.

---

## 5. The priority ladder (synthesised from all sources)

**T0 — on login (0-30s, P0, never fails)**
1. Auto-reconnect / recover drop
2. Shield check → refresh under threshold
3. Anti-scout check → refresh under threshold
4. Shelter check → re-shelter near expiry
5. Threat eval: attacked / scouted / conflict-march on any gathering tile → recall now + set regather cooldown
6. Revive dead leader (removes all boosts)

**T1 — free claimables (0-2 min, before spending anything)**
7. Daily Login Gift · 8. Mystery Box · 9. VIP/free/artifact chests · 10. Alliance gifts + delete
11. **Colosseum gems** (collect before anything can spend them)
12. Guild Fest rewards → **Gems → MH Chest → random**
13. Treasure Trove maturity · 14. all auto-completable quests

**T2 — economy burst (2-10 min)**
15. Guild help — only if the bar isn't full; 7-day boosts before 24h; then **slow down** so others can fill theirs
16. Supply dump above reserve down to reserve, from bag, travel-time capped, slight randomisation
17. Buildspam if guild coins are the bottleneck

**T3 — sustained production (continuous, runs all session)**
18. Gathering: lowest-resource-type first → highest level tile → closest / max travel time. Leave one spare army.
19. Building queue always full, lowest level first, priority ladder

**T4 — time-gated spenders (fire the instant the gate opens)**
20. Research — only if none in progress; target system; Technolabe only above the might threshold and never during a scoring event
21. Colosseum — only while free attempts remain and win chance is in band; buy extra **only** after all free are spent
22. Hero Stages — gated on stamina; 10× sweep at ≥100; Braveheart at 0; Elite for medals, Normal for trophies
23. Monster hunt — gated on energy **% of max**, not >0; Steal (<20% HP → x2) preferred; combo prediction
24. Familiars · 25. Cargo/Labyrinth/Tycoon/Verge/Mystic Spire/Artifacts (8h cadence)

**T5 — troop lifecycle**
26. Train per current chapter, rotating · 27. Heal infirmary; sanctuary in 25% chunks · 28. Attack skirmish at N% trained

**T6 — event-scoped**
29. Guild Fest (point-range filter) · 30. Showdown / Chaos / Dragon Arena / Expedition auto join-leave · 31. KvK

**Cadence table**: ~15min mystery box · ~2h re-shelter · ~8h artifact check + colosseum window · 3×/day RSS supply · daily login/attempts/guild · ~30d trove.
