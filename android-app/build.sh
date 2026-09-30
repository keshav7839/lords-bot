#!/usr/bin/env bash
# Standalone APK build for lordsM.
#
# No Gradle, no Android Studio, no dependency resolution: javac + kotlinc +
# d8 + aapt + apksigner, which is what makes this runnable on a machine
# that has no working SDK layout (and on ARM64, where aapt2/d8 host
# binaries do not exist).
#
# Everything it needs is passed in through the environment, so the same
# script works here and on a normal x86-64 CI box.
set -euo pipefail

: "${JAVA_HOME:?set JAVA_HOME}"
: "${ANDROID_JAR:?set ANDROID_JAR (e.g. /sdk/platforms/android-35/android.jar)}"
: "${D8_JAR:?set D8_JAR (e.g. build-tools/*/lib/d8.jar)}"
: "${APKSIGNER_JAR:?set APKSIGNER_JAR}"
: "${AAPT:?set AAPT (aapt v1, does compile+link and emits R.java)}"
: "${KOTLINC:?set KOTLINC}"
: "${KOTLIN_STDLIB:?set KOTLIN_STDLIB}"

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/app/src/main"
OUT="$HERE/build"
APK_NAME="${APK_NAME:-lordsm.apk}"

export PATH="$JAVA_HOME/bin:$PATH"
# Termux's JDK needs libandroid-spawn.so, which no current Termux package
# ships; a stub satisfies the NEEDED entry and the real symbols come from
# bionic. Harmless elsewhere.
# The JDK's own libs (libjli.so for javac, libjava.so for java) plus, on
# Termux, a stub libandroid-spawn.so that no current package ships.
export LD_LIBRARY_PATH="$JAVA_HOME/lib:$JAVA_HOME/lib/server:/tmp/opencode/tools/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

rm -rf "$OUT"
mkdir -p "$OUT/gen" "$OUT/classes" "$OUT/kt" "$OUT/dex"

say() { printf '\n\033[1;33m== %s\033[0m\n' "$*"; }

say "1/7 native engine -> lib/arm64-v8a/libbot.so"
mkdir -p "$OUT/dex/lib/arm64-v8a"
# The engine is the same C the desktop build uses; jni.c adds the bridge.
gcc -O2 -fPIC -shared -DVERSION='"1.0.2"' -D_GNU_SOURCE \
    -I"$HERE/native/bot" -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
    -o "$OUT/dex/lib/arm64-v8a/libbot.so" \
    "$APP/cpp/jni.c" \
    "$HERE"/native/bot/{main,connection,log,protocol,des,map_point,command,config,auto,waves,bank}.c \
    -llog
echo "   built: $(ls -l "$OUT/dex/lib/arm64-v8a/libbot.so" | awk '{print $5}') bytes"

say "2/7 resources -> R.java + resource-only APK"
"$AAPT" package -f -m \
    -J "$OUT/gen" \
    -M "$APP/AndroidManifest.xml" \
    -S "$APP/res" \
    -I "$ANDROID_JAR" \
    -F "$OUT/res-only.apk"

say "3/7 javac (R.java)"
# -bootclasspath is rejected alongside -source/-target 17; android.jar goes
# on the regular classpath instead. R.java only touches android.R and
# android.content res ids, so that is sufficient.
javac -nowarn -proc:none -implicit:none \
    -classpath "$ANDROID_JAR" \
    -d "$OUT/classes" $(find "$OUT/gen" -name '*.java')

say "4/7 kotlinc"
mapfile -t KTS < <(find "$APP/java" -name '*.kt')
"$KOTLINC" "${KTS[@]}" \
    -classpath "$ANDROID_JAR:$KOTLIN_STDLIB:$OUT/classes" \
    -d "$OUT/kt" -nowarn 2>&1 | grep -vE "^(warning:|info:)" || true

say "5/7 d8 -> classes.dex"
CLASSES=$(find "$OUT/classes" "$OUT/kt" -name '*.class')
STDLIB_DIR="$OUT/std"
mkdir -p "$STDLIB_DIR"
(cd "$STDLIB_DIR" && unzip -qo "$KOTLIN_STDLIB" 'kotlin/**' 2>/dev/null || true)
java -cp "$D8_JAR" com.android.tools.r8.D8 \
    --min-api 26 --lib "$ANDROID_JAR" \
    --output "$OUT/dex" $CLASSES \
    $(find "$STDLIB_DIR" -name '*.class' | head -400)

say "6/7 assemble APK"
cp "$OUT/res-only.apk" "$OUT/$APK_NAME"
cp "$OUT/dex/classes.dex" "$OUT/"
(cd "$OUT/dex" && find lib -type f -exec cp --parents {} "$OUT/" \;)
# zipalign is an x86-64 host binary and is not required for installation -
# the APK is simply not page-aligned. `zip` may also be absent, so append
# with python's zipfile instead of shelling out.
python3 - "$OUT/$APK_NAME" "$OUT" <<'PYEOF'
import sys, zipfile, os
apk, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(out, 'classes.dex'), 'classes.dex')
    z.write(os.path.join(out, 'lib/arm64-v8a/libbot.so'), 'lib/arm64-v8a/libbot.so')
PYEOF

say "7/7 sign"
KS="$OUT/debug.ks"
keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias lordsm -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=lordsM, OU=dev, O=lordsM, L=-, S=-, C=ZZ" >/dev/null 2>&1
java -cp "$APKSIGNER_JAR" com.android.apksigner.ApkSignerTool sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias lordsm --min-sdk-version 26 \
    --out "$OUT/$APK_NAME.signed" "$OUT/$APK_NAME"
mv "$OUT/$APK_NAME.signed" "$OUT/$APK_NAME"

say "done"
ls -l "$OUT/$APK_NAME"