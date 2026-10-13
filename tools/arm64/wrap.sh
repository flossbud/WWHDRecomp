#!/usr/bin/env bash
# tools/arm64/wrap.sh DIR [BIN]: DIR/wwhd-null runs the arm64 build (BIN, default build/arm64/wwhd-null) under qemu-user,
# so the x86 tests take it as their CEMU_BIN unchanged (m-android A1):
#   tools/arm64/wrap.sh /wwhd/tmp/arm64-bin && CEMU_BIN=/wwhd/tmp/arm64-bin/wwhd-null tools/reference/route.sh ...
# The binary sits beside the wrapper as wwhd-null.arm64 (qemu gives it its own path for /proc/self/exe, so its
# folder is the emulator dir as for any CEMU_BIN). Expect it 10-30 times slower than native.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=${1:?DIR}
bin=${2:-$root/build/arm64/wwhd-null}
sysroot=${ARM64_SYSROOT:-/wwhd/opt/arm64/root}
mkdir -p "$dir"
cp "$bin" "$dir/wwhd-null.arm64"
cat > "$dir/wwhd-null" <<WRAP
#!/usr/bin/env bash
exec "$sysroot/usr/bin/qemu-aarch64" -L "$sysroot" "\$(dirname "\$0")/wwhd-null.arm64" "\$@"
WRAP
chmod +x "$dir/wwhd-null"
echo "arm64: $dir/wwhd-null runs $bin under qemu"
