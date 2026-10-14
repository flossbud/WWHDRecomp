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
# the app's own files (t-startscreen S3: a player has no device.sh files): app/ is ours, extracted to the files dir's
# app/ and replaced on an update (Cemu's resources and game profiles with the game's own as its default, the shader
# list, the pause menu's assets: WWHD_CEMU_DATA and WWHD_UI_ASSETS point there); once/ is the player's once there
# (Cemu's settings.xml, the controller profile), copied into the files dir only where missing. WindSongActivity
# extracts them when the trees' hash (wwhd-assets.txt) differs from the last
a=$w/assets
mkdir -p "$a/app/cemu/wwhd" "$a/app/assets" "$a/once/portable/controllerProfiles"
cp -r /wwhd/opt/cemu-src/bin/resources /wwhd/opt/cemu-src/bin/gameProfiles "$a/app/cemu/"
cp "$root/tools/reference/0005000010143500.ini" "$a/app/cemu/gameProfiles/default/"
cp "$root/config/US_v0/shader_list.txt" "$a/app/cemu/wwhd/"
cp -r "$root/assets/pause_menu" "$a/app/assets/"
cp "$root/tools/reference/settings.xml" "$a/once/portable/"
cp "$root/tools/reference/controller0.xml" "$a/once/portable/controllerProfiles/"
(cd "$a" && find app once -type f | LC_ALL=C sort | xargs -d '\n' sha256sum | sha256sum | cut -c1-16) > "$a/wwhd-assets.txt"
"$bt/aapt2" link -I "$jar" --manifest "$root/android/AndroidManifest.xml" -o "$w/base.apk" "$w/res.zip" -A "$a" \
    --min-sdk-version 33 --target-sdk-version $api --debug-mode   # debuggable: run-as for maps, simpleperf (A3)
llvm-objcopy --strip-debug "$lib" "$w/lib/arm64-v8a/libmain.so"
cp "$w/base.apk" "$w/unaligned.apk"
(cd "$w/dex" && zip -q -X "$w/unaligned.apk" classes.dex)
# libadrenotools' hooks (a4-drivers), loaded from the native library dir the installer extracts them to: from
# ADRENOTOOLS_HOOKS, else LIB's build dir's, else build/android's (a LIB copied elsewhere has none beside it); without
# them an imported GPU driver doesn't load and the system's is used
hooks=${ADRENOTOOLS_HOOKS:-$(dirname "$lib")/src/adrenotools/src/hook}
[ -d "$hooks" ] || hooks=$root/build/android/src/adrenotools/src/hook
n=0
for h in hook_impl main_hook file_redirect_hook gsl_alloc_hook; do
    f=$hooks/lib$h.so
    [ -f "$f" ] && llvm-objcopy --strip-debug "$f" "$w/lib/arm64-v8a/lib$h.so" && n=$((n + 1))
done
[ $n -eq 4 ] || echo "android: warning: $n of libadrenotools' 4 hooks in $hooks (an imported GPU driver won't load)" >&2
(cd "$w" && zip -q -X -0 "$w/unaligned.apk" lib/arm64-v8a/*.so)
"$bt/zipalign" -f -P 16 4 "$w/unaligned.apk" "$w/aligned.apk"
ks=$sdk/debug.keystore
[ -f "$ks" ] || keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias debug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug" >/dev/null 2>&1
"$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android --out "$apk" "$w/aligned.apk"
echo "android: $apk ($(du -h "$apk" | cut -f1))"
