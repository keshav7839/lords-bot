# Deep protocol reference (no payloads sent)

Source of truth: a 3413-file decompile of the game client
(`Assembly-CSharp`), read directly. This document only records what the
client declares. **No packet was sent to obtain any of it** - every
item below is either a struct schema, a constant, or a push the server
already volunteers at login.

## Where the values are NOT

Every table is a `StructLayout(Pack = 1)` struct with no literals. The
per-level numbers (costs, times, prerequisites) are loaded at runtime by
`CExternalTableWithWordKey<T>` from asset bundles shipped inside the game
client. The APK available on this device is **not** the game - its
`assets/app-config.json` reads `"appName":"Surprise Gift For You"`,
`"packageName":"com.surprisegiftforyou"`, a webview gift shell. That is
also, definitively, why an earlier search of `classes.dex` found zero of
`LordEquip`, `MysticSpire`, `Pact`, `Artifact`, `Talent`, ...: they were
never in that file, in any form.

Consequence: costs and prerequisites can be **declared** but not
**valued** from what we hold. Anything that needs a real cost number must
read it off the server or off a real client's assets.

## Building

### `BuildLevelRequest` - the upgrade record (5+ field schema)
`u16 ID, u16 BuildID, u8 Level, u32 BuildTime, u16 GroupID,
u32 RequestFood, RequestRock, RequestWood, RequestIron, RequestGold,
u32 CastleArmy, u32 Strength, u16 Effect1, u32 Value1, u16 Effect2,
u32 Value2, u16 Effect3, u16 Value3, u16 Effect4, u16 Value4, u16 Value5,
u16 ExtEffect1, u32 ExtValue1, u16 ExtEffect2, u8 ExtValue2`

Note the five separate resource costs and `CastleArmy` - a building can
require a troop *count*, not just resources. This is almost certainly
why upgrades are being refused (see below).

### `RoleBuildingData` - current state (what we parse)
`u16 ManorID, u16 BuildID, u8 Level` = 5 bytes.

**Verified against our parser:** `RecvAllBuildData` reads
`u8 count` then 5 bytes per record, exactly this. The 66-building census
and the upgrade target selection were therefore reading real state.

### `BuildTypeData`
`u16 BuildID, u16 NameID, u8 Kind, u8 GraphicID, u16 StringID,
u16 ContentID, u16 UIExplain`

### `eBuildState` (client-side state machine)
`None, OpenUI, Building, Complete, Cancel, Prompt, Login, TurnOffEff,
CheckUpdateResource, CheckUpdateCondition, TurnOffArrow, CheckPrisoner,
CustomCastle`

`CheckUpdateResource` and `CheckUpdateCondition` are the client-side
hints that an upgrade can fail for want of resources *or* of an unmet
condition - the two reasons to expect `error=3` below.

## Research

### `TechLevelTbl` - this is the prerequisite graph
`u16 ID, u16 TechID, u8 Level, u32 LevelupTime, u32 Grain, u32 Rock,
u32 Wood, u32 Iron, u32 Gold, u8 ResearchLevel,
u16 RequireTechID1, u8 RequireTechLv1, ... RequireTechID2/Lv2,
RequireTechID3/Lv3, RequireTechID4/Lv4, u32 Strength, u16 Effect,
u32 EffectVal`

Two things the bot was missing:

1. **`ResearchLevel`** - each tech is gated by a level of the research
   building itself. A tech below the lab's current level is not a valid
   target no matter how cheap it looks.
2. **Up to four** prerequisite techs, each with its own required level.

The current `ResearchTick` blind sweep over ids 1-400 cannot express
either. It needs the *values* of `TechID`/`ResearchLevel`, which is the
one thing this decompile does not carry.

### `TechLevelExTbl` - variable-length tail
`u16 ID, u16 TechID, u8 Level, u32 PetResource, u32[] Reserve`

`Reserve` is a marshalled array, so **any research-info parse must handle
a variable-length tail**. A fixed-size reader will silently mis-align.

## Other tables

| Struct | Fields |
| --- | --- |
| `TalentLevelTbl` | `u16 ID, u16 TalentID, u8 Level, u8 NeedPoint, u16 Effect, u16 EffectVal` |
| `Level` (stage) | `u16 LevelKey, u16[] Team, u16 TreasureNo, u16 LevelInfoNo, u16 LeadLV, u16 Money, u16 TalkBefore, u16 TalkAfter` |
| `HeroTrainExpTbl` | `u16 ID, u16 PetEep, u16 PetSkillExp` (the `PetEep` typo is the game's) |
| `MapTileLevel` | constants `rankone = 113`, `darkrankone = 256` |

`TalentLevelTbl.NeedPoint` is the talent-point cost per level, so talent
spends are a point-budget problem, not a resource problem - which makes
talent the cheapest remaining feature to automate.

## Pushes the server volunteers (115 distinct, observed)

Recorded from one clean run. Unparsed and directly useful:

| Opcode | Name | Why it matters |
| --- | --- | --- |
| 3801 | `TALENTINFO` | talent state; pairs with 3802 to make talent automatable |
| 4001/4026/4041/4053/4056/4068 | `TREASURE_LIST*` | the whole treasure/pop-up reward chain |
| 9771 / 9795 | `RELICS_INFO`, `RELICS_RANKGACHA_LIST` | a whole progression system we ignore |
| 11352 | `SET_AUTO_DARK_NEST` | darknest auto-config is server-side |
| 3140 / 9402 | `VALHALLA_MISSION`, `VALHALLA_INFO` | Valhalla / kingdom pass |
| 2601 | `WALLINFO` | wall state alongside the traps we do read |
| 3187 | `RESCUE_BEAUTY_INFO` | unhandled event |
| 5401 | `WONDER_INIT_NOTICE` | wonder state |
| 1821 | `SPCHALLENGE2_INFO` | arena/challenge state |

Every one of these is **free to use**: the server sends them unprompted,
so parsing them carries no session risk. This is the cheapest remaining
source of new capability.

## Open blocker: every upgrade is refused with error=3

A clean run made 6 upgrade requests and got 6 rejections:

```
[INFO ] [BUILD] upgrade Castle (id=8) pos=1265 level=23 -> 24
[INFO ] [BUILD] server rejected upgrade (error=3), rotating list
... 6 attempts, 6 x error=3
```

This corrects an earlier claim in this repo that building upgrades were
"live". What was logged was the *decision* line, not server acceptance.
The upgrades never happened.

The client has no error enum for this - `Recv_MSG_RESP_BUILDBEGIN` only
tests the byte against zero, so `3` carries no documented meaning. Given
`BuildLevelRequest.CastleArmy` and the `CheckUpdateResource` /
`CheckUpdateCondition` states, the two live candidates are:

- **not enough of a resource** (food/rock/wood/iron/gold), or
- **an unmet condition**, e.g. the `CastleArmy` troop requirement, or the
  manor/level gate.

To tell them apart without guessing, the bot needs either the account's
current resource stocks (never logged - only market items are) or the
upgrade cost/precondition for the target level. The cost comes from
`BuildLevelRequest`, whose *values* we do not have. So the honest next
step is to log resource state from the pushes that carry it and compare
against a known-good manual upgrade, not to keep rotating the list.
