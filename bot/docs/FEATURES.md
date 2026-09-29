
## Protocol shapes confirmed from a full game decompile

The game code is in the native library, not `classes.dex` (all target
strings were absent there), so the payload layouts were read out of a
real 3413-file decompile of `Assembly-CSharp`
(`supdams/Lords-Mobile-Bot-SupDams`). Every shape below is copied from
the client-side builder, not inferred.

| Opcode | Name | Body after `seq` |
| --- | --- | --- |
| 1416 | `LOADEQUIP` | `i64 lastUpdateTime` (0 = full load) |
| 3805 | `PUTON/TAKEOFF_LORDEQUIP` | `u8 equipPos`, `u32 serial` |
| 3809 | `SYN_LORDEQUIP` | `u16 itemID`, `u32 serial` |
| 1204 | `HEROENHANCE` | `u32 heroID` |
| 3802 | `TALENT_LEVEL_ADD` | opcode known, body not in this build |
| 8204 | `PET_TRAINING_BEGIN` | `u8 gymSlot`, `u16 familiarID`, `u8 heroCount`, `u16 heroID` xN |
| 8216 | `ITEMCRAFT_START` | `u16 craftID`, `u16 count`, `u8 instantComplete` |

Pushes now parsed that were previously discarded:

| Opcode | Layout | Live result |
| --- | --- | --- |
| 3804 | 8 slots x 27B: `u16 itemID`, `u8 color`, `4x u8 gemColor`, `4x u16 gemID`, `u32 serial` | `7 of 8 slots equipped: slot0=item4746/q2/s1 ...` |
| 8216 | variant-tagged craft list, `0` = type 0 | craft queue empty (all-zero payload) |

### Fifth rejected opcode: 1416

Asking for the equipment inventory drops the session on this server, the
same as `1004`, `2000`, `2405` and `2852`. The body is confirmed from the
client, so this is a server-side refusal rather than a malformed packet.
`wave.load_equip_inventory` is therefore off by default. The worn-gear
state from `3804` is parsed either way, so `$herolv` and any future swap
work without it.

### New manual commands

- `$herolv <heroId>` - level a hero up once (1204). Manual by design: it
  spends the account's own hero XP books, so the spend is the player's
  decision. The gem-costing instant finish (1206) is not implemented.
