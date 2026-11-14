#!/usr/bin/env bash
# Runs inside the tizen-steam-link-toolchain container: compiles the PNaCl module
# (pepper_63), translates it to ARM and x86-32 .nexe, signs and packs TizenSteamLink.wgt.
set -euo pipefail

ROOT=/src
BUILD_DIR="${BUILD_DIR:-$ROOT/build/nacl}"
OUT_DIR="$ROOT/out"

echo "==> Building NaCl module (pepper_63) into $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/PNaClToolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

for f in tsl-arm.nexe tsl-x86-32.nexe tsl.nmf; do
    if [ ! -f "$ROOT/web/$f" ]; then
        echo "ERROR: expected $ROOT/web/$f after build" >&2
        exit 1
    fi
done

echo "==> Staging and signing .wgt"
STAGE=/tmp/tsl-stage
rm -rf "$STAGE"
mkdir -p "$STAGE/widget" "$OUT_DIR"
cp -r "$ROOT/web/." "$STAGE/widget/"
rm -rf "$STAGE/widget/dev" "$STAGE/widget/tsl.js" "$STAGE/widget/tsl.wasm" "$STAGE/widget/tsl.worker.js"

expect <<EOF
set timeout 300
spawn tizen package -t wgt -s TSL -- $STAGE/widget
expect {
    "Author password:" { send "1234\r"; exp_continue }
    "Yes: (Y), No: (N) ?" { send "N\r"; exp_continue }
    eof
}
catch wait result
exit [lindex \$result 3]
EOF

wgt=$(ls "$STAGE/widget"/*.wgt | head -n 1)
mv "$wgt" "$OUT_DIR/TizenSteamLink.wgt"

echo "==> Wrote $OUT_DIR/TizenSteamLink.wgt ($(du -h "$OUT_DIR/TizenSteamLink.wgt" | cut -f1))"
