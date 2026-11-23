#!/usr/bin/env bash
# Asteroid Fusion (HMY AFX) -> APK Android native murni (NativeActivity, android:hasCode="false").
# NDK clang -> aapt2 (resource + ikon + assets) -> zipalign (16 KB) -> apksigner.
# Tanpa Gradle, tanpa javac, tanpa d8/dex: APK tidak memuat classes.dex.
#
# Pemakaian:  android/build.sh            # arm64-v8a + armeabi-v7a + x86_64
#             ABIS="arm64-v8a" android/build.sh   # satu ABI saja (cepat)
#             CLEAN=1 android/build.sh    # bangun ulang semua objek
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

SDK=${ANDROID_HOME:-/opt/android-sdk}
NDK=$SDK/ndk/28.1.13356709
BT=$SDK/build-tools/36.0.0
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
GLUE=$NDK/sources/android/native_app_glue
RL=vendor/raylib/src
PLATFORM_JAR=$SDK/platforms/android-34/android.jar
API=26
TARGET=34
VNAME=$(sed -n 's/^#define AFX_VERSION "\(.*\)"/\1/p' src/afx.h)
VCODE=${VCODE:-3}
ABIS=${ABIS:-"arm64-v8a armeabi-v7a x86_64"}
OUT=build-android
DIST=dist
JOBS=$(nproc)

[ "${CLEAN:-0}" = 1 ] && rm -rf "$OUT"
rm -rf "$OUT/stage" "$OUT/res.zip" "$OUT"/*.apk
mkdir -p "$OUT/stage" "$DIST"

triple() { case $1 in
  arm64-v8a) echo aarch64-linux-android ;;
  armeabi-v7a) echo armv7a-linux-androideabi ;;
  x86_64) echo x86_64-linux-android ;;
  *) echo "ABI tidak dikenal: $1" >&2; exit 1 ;; esac; }

RL_SRC="rcore.c rshapes.c rtextures.c rtext.c rmodels.c raudio.c utils.c"
LUA_SRC=$(ls vendor/lua/*.c | grep -vE '/(lua|luac|ltests|onelua)\.c$')
COMMON="-O2 -fPIC -ffunction-sections -fdata-sections -DNDEBUG"
RL_FLAGS="-std=gnu99 $COMMON -I$GLUE -I$RL -D__ANDROID__ -DPLATFORM_ANDROID -DGRAPHICS_API_OPENGL_ES2 -w"
LUA_FLAGS="-std=gnu99 $COMMON -Ivendor/lua -DLUA_USE_POSIX -w"
AFX_FLAGS="-std=gnu11 $COMMON -DPLATFORM_ANDROID -Isrc -Ivendor/lua -Ivendor/nanosvg -Ivendor/stb -I$RL -I$GLUE -Wall -Wno-unused-function -Wno-unused-variable"
LDFLAGS="-shared -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none -Wl,--no-undefined -Wl,-z,relro,-z,now \
 -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"
LIBS="-landroid -lEGL -lGLESv2 -lOpenSLES -llog -lm -ldl -lmediandk"

build_abi() {
  local abi=$1 cc=$TC/$(triple "$1")$API-clang o=$OUT/obj/$1
  mkdir -p "$o/rl" "$o/lua" "$o/afx" "$OUT/stage/lib/$abi"
  : > "$o/cmds.txt"
  # raylib + Lua: dibuat sekali, dipakai ulang antar-build
  for f in $RL_SRC; do [ -f "$o/rl/${f%.c}.o" ] || echo "$cc $RL_FLAGS -c $RL/$f -o $o/rl/${f%.c}.o" >> "$o/cmds.txt"; done
  [ -f "$o/rl/glue.o" ] || echo "$cc -std=gnu99 $COMMON -w -I$GLUE -c $GLUE/android_native_app_glue.c -o $o/rl/glue.o" >> "$o/cmds.txt"
  for f in $LUA_SRC; do b=$(basename "$f" .c); [ -f "$o/lua/$b.o" ] || echo "$cc $LUA_FLAGS -c $f -o $o/lua/$b.o" >> "$o/cmds.txt"; done
  # kode AFX: selalu dikompilasi ulang
  rm -f "$o"/afx/*.o
  for f in src/*.c; do b=$(basename "$f" .c); echo "$cc $AFX_FLAGS -c $f -o $o/afx/$b.o" >> "$o/cmds.txt"; done
  echo "[$abi] kompilasi $(wc -l < "$o/cmds.txt") berkas"
  xargs -P "$JOBS" -I{} sh -c "{}" < "$o/cmds.txt"
  $cc $LDFLAGS "$o"/afx/*.o "$o"/lua/*.o "$o"/rl/*.o $LIBS -o "$OUT/stage/lib/$abi/libmain.so"
  echo "[$abi] libmain.so $(du -h "$OUT/stage/lib/$abi/libmain.so" | cut -f1)"
}
for abi in $ABIS; do build_abi "$abi"; done

# ---- resource (ikon) + assets (font) + manifest ----
[ -d android/res ] || python3 android/make_icon.py
$BT/aapt2 compile --dir android/res -o "$OUT/res.zip"
$BT/aapt2 link --auto-add-overlay -o "$OUT/base.apk" --manifest android/AndroidManifest.xml -I "$PLATFORM_JAR" -R "$OUT/res.zip" \
  -A assets --min-sdk-version $API --target-sdk-version $TARGET --version-code "$VCODE" --version-name "$VNAME"
cp "$OUT/base.apk" "$OUT/unaligned.apk"
(cd "$OUT/stage" && zip -0 -X -r ../unaligned.apk lib >/dev/null)
$BT/zipalign -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

# ---- tanda tangan (kunci dibuat sekali; kata sandi acak disimpan di android/keystore.pass) ----
KS=android/release.keystore; PASS=android/keystore.pass
if [ ! -f "$KS" ]; then
  head -c 18 /dev/urandom | base64 | tr -d '/+=' > "$PASS"; chmod 600 "$PASS"
  keytool -genkeypair -keystore "$KS" -storepass "$(cat $PASS)" -keypass "$(cat $PASS)" -alias hmyafx \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=HMY AFX, O=HMY Software" >/dev/null 2>&1
  chmod 600 "$KS"
fi
APK=$DIST/AsteroidFusion-$VNAME.apk
export AFX_KS_PASS=$(cat "$PASS")
$BT/apksigner sign --ks "$KS" --ks-pass env:AFX_KS_PASS --key-pass env:AFX_KS_PASS \
  --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true --out "$APK" "$OUT/aligned.apk"

# ---- verifikasi ----
echo "== verifikasi"
$BT/apksigner verify --verbose "$APK" | head -4
$BT/zipalign -c -P 16 4 "$APK" && echo "zipalign 16KB: OK"
if unzip -l "$APK" | grep -q "classes.*\.dex"; then echo "GAGAL: ada classes.dex" >&2; exit 1; else echo "tanpa dex: OK"; fi
$BT/aapt2 dump badging "$APK" | grep -E "^(package|sdkVersion|targetSdkVersion|application-label:|application-icon-640|launchable|native-code|uses-feature)" | head -9
echo "BUILD OK -> $ROOT/$APK ($(du -h "$APK" | cut -f1))"
