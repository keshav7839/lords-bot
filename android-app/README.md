# Lords Bot Panel

An Android control panel for the C bot in `native/bot`. The panel hosts the
existing engine as `libbot.so`; it does not reimplement it.

## The build

`./build.sh` produces `build/lordsm.apk`. **No Gradle, no Android Studio, no
dependency resolution** — just `javac` + `kotlinc` + `d8` + `aapt` +
`apksigner`:

```
export JAVA_HOME=... ANDROID_JAR=.../android.jar D8_JAR=.../d8.jar \
       APKSIGNER_JAR=.../apksigner.jar AAPT=.../aapt \
       KOTLINC=.../kotlinc KOTLIN_STDLIB=.../kotlin-stdlib.jar
./build.sh
```

This runs identically on ARM64 and x86-64. It matters because the usual
toolchain does not: `aapt2`, `d8`, `zipalign` and `apksigner`'s helper ship
as **x86-64 host binaries**, so an APK normally cannot be produced on an
ARM64 phone at all. `build.sh` sidesteps `aapt2` by using **aapt v1**, which
compiles and links resources in one pass and still emits `R.java`, and skips
`zipalign` entirely (the APK is simply not page-aligned, which costs a little
size and does not affect installation).

Signing uses a generated debug key. Swap in a real key before distributing.

### Why the UI is framework-only

There is no AndroidX and no Compose in this app. The UI is plain `Activity`,
`ListView`, `Switch`, `EditText` and `Spinner` built in Kotlin. Every
third-party dependency removed is one fewer thing that can fail to resolve on
a machine with no Gradle cache, and the trade was worth it: the first end-to-end
build succeeded. The cost is a less decorative UI than Compose would give.

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
