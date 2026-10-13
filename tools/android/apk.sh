#!/usr/bin/env bash
# tools/android/apk.sh [LIB [OUT_APK]]: the app's APK (m-android A2), on the worker after tools/android/build.sh,
# without Gradle: SDL's Java (from the SDL release vcpkg's sdl3 is, /wwhd/opt/dl) and android/java compiled against
# the platform with javac, dexed by d8; android/'s manifest and resources linked by aapt2; LIB (default
# build/android/libwwhd-play.so) as lib/arm64-v8a/libmain.so, stored uncompressed and 16K-aligned (zipalign -P 16) so
# it's mapped from the APK; signed with a debug key made once in /wwhd/opt/android (never in git). The APK is
# generated output: never in git.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_DIR:-/wwhd/opt/android}
lib=${1:-$root/build/android/libwwhd-play.so}
apk=${2:-$root/build/android/windsong.apk}
api=35; bt=$sdk/build-tools/$api.0.0; jar=$sdk/platforms/android-$api/android.jar
sdl=SDL-release-3.4.10
w=$root/build/android/apk
rm -rf "$w" && mkdir -p "$w/src" "$w/classes" "$w/dex" "$w/lib/arm64-v8a"
tar -xzf /wwhd/opt/dl/libsdl-org-$sdl.tar.gz -C "$w/src" --strip-components=6 "$sdl/android-project/app/src/main/java"
cp -r "$root/android/java/." "$w/src/"
javac -nowarn --release 17 -classpath "$jar" -d "$w/classes" $(find "$w/src" -name '*.java') 2>&1 | grep -v '^Note:' || true
"$bt/d8" --release --min-api 33 --lib "$jar" --output "$w/dex" $(find "$w/classes" -name '*.class')
"$bt/aapt2" compile --dir "$root/android/res" -o "$w/res.zip"
"$bt/aapt2" link -I "$jar" --manifest "$root/android/AndroidManifest.xml" -o "$w/base.apk" "$w/res.zip" \
    --min-sdk-version 33 --target-sdk-version $api
llvm-objcopy --strip-debug "$lib" "$w/lib/arm64-v8a/libmain.so"
cp "$w/base.apk" "$w/unaligned.apk"
(cd "$w/dex" && zip -q -X "$w/unaligned.apk" classes.dex)
(cd "$w" && zip -q -X -0 "$w/unaligned.apk" lib/arm64-v8a/libmain.so)
"$bt/zipalign" -f -P 16 4 "$w/unaligned.apk" "$w/aligned.apk"
ks=$sdk/debug.keystore
[ -f "$ks" ] || keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias debug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug" >/dev/null 2>&1
"$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android --out "$apk" "$w/aligned.apk"
echo "android: $apk ($(du -h "$apk" | cut -f1))"
