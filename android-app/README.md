# Lords Bot Panel

An Android control panel for the C bot in `native/bot`. The panel hosts the
existing engine as `libbot.so`; it does not reimplement it.

## Why the APK is built in CI

`aapt2`, `d8` and `apksigner` ship as **x86-64 host binaries**. An APK cannot
be produced on an ARM64 device, so `.github/workflows/apk.yml` builds it on an
x86-64 runner and publishes it as a release artifact. If you open the project
in Android Studio it builds the same way locally.

Release signing uses `BOT_KEYSTORE_B64` / `BOT_KEYSTORE_PASS` /
`BOT_KEY_ALIAS` / `BOT_KEY_PASS` secrets. **Without them the release build
falls back to the debug key**, so a fork still produces an installable APK —
just not one from Play.

## Layout

```
app/src/main/cpp/jni.c        JNI bridge: engine thread + log pipe
app/src/main/cpp/CMakeLists.txt
native/bot/                   the C engine, byte-identical to the desktop build
app/src/main/java/.../feature/FeatureRegistry.kt   every control, with a risk tier
app/src/main/java/.../ConfigWriter.kt               renders config + enforces mode
app/src/main/java/.../pcap/PcapdroidImporter.kt    credential import
```

## Safety modes

Every setting carries a risk tier, decided by what that opcode actually did to
the session when it was tried for real:

| Tier | Meaning |
| --- | --- |
| `SAFE` | read-only, or proven against the decompiled client and observed working |
| `CAUTION` | proven opcode, but state-changing — it spends the account's own resources |
| `RISKY` | never proven, or proven to get the session closed |

**Ultra Safe Mode** pins every RISKY key to `false`. The clamp is applied in
`ConfigWriter` when the config file is written — *not* in the UI — so a screen
that forgets to check cannot enable a forbidden feature.

`RiskyKeys.ALWAYS_BLOCKED` lists the twelve keys that are off in **every**
mode. These are the opcodes empirically found to close the session:

| Key | Opcode | Why |
| --- | --- | --- |
| `wave.request_role_info` | 1004 | `_MSG_LOGIN_REQUESTLOGIN` — a *re-auth attempt*, not a data request. Removed from the C source entirely. |
| `wave.request_build_info` | 2000 | rejected |
| `wave.load_equip_inventory` | 1416 | rejected (body is confirmed; the server refuses it) |
| `train.dismiss_above` | 2405 | rejected |
| `train.instant_finish` | 2407 | `FINISHTRAINING` — spends gems |
| `alliance.request_own_help` | 2852 | rejected |
| `wave.heal_troops` | — | payload unconfirmed; closes the session |
| `protection.shield_*`, `speedup.enabled` | — | limited resources |
| `wave.stage_sweep` | — | incomplete payload |

## Credential import

Accounts screen → **Import** → pick a PCAPdroid **text** export of the game's
login exchange. The importer matches the access-key token in the export text
rather than reproducing byte offsets, because PCAPdroid renders unprintable
bytes differently and offset arithmetic broke on that before.

## Not included, on purpose

- **No overlay / floating panel over the game.** Drawn-over-game panels styled
  like mod menus are a detection-evasion surface; this panel uses a normal
  notification and an in-app view.
- **No accessibility-service automation.** The bot reads game state over its
  own socket. It never drives the game's UI.
- **No session harvesting from the game app.** Import a capture you took
  yourself.

## Known-broken, honestly

Building upgrades and troop training both currently get refused by the server
(`error=3` and `err=2`). Both payloads have been checked against the
decompiled client and are correct; the refusal is unexplained and the working
theory is that the account cannot afford either. The bot cannot read its own
resource stocks to confirm, because those live in the `ROLEINFO` payload.

A bot like this automates a live multiplayer account, which is against the
game's terms and risks the account. That is your call to make knowingly.
