#!/bin/sh
# Cross-compiles the Steam Frame (aarch64) binaries with zig, from any OS that
# has zig (https://ziglang.org) or `pip install ziglang`. Output: linux/dist/
#   VULKAN_INCLUDE=/path/to/Vulkan-Headers/include sh linux/build.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/dist/oal-frame"
zigcc="${ZIG:-zig}"
if ! command -v "$zigcc" >/dev/null 2>&1; then zigcc="python3 -m ziglang"; fi
vk="${VULKAN_INCLUDE:-/usr/include}"
[ -f "$vk/vulkan/vk_layer.h" ] || { echo "Set VULKAN_INCLUDE to a Vulkan-Headers include directory" >&2; exit 1; }
target=aarch64-linux-gnu.2.28   # works on any glibc >= 2.28
flags="-target $target -std=c++17 -O2 -Wall -Wno-missing-field-initializers"
mkdir -p "$out/lib" "$out/bin" "$here/dist/.vkinc"
# Copy only the Vulkan headers so host system headers never leak into the build.
cp -R "$vk/vulkan" "$here/dist/.vkinc/"
[ -d "$vk/vk_video" ] && cp -R "$vk/vk_video" "$here/dist/.vkinc/"
$zigcc c++ $flags -I"$here/dist/.vkinc" -fPIC -shared -fvisibility=hidden \
    -Wl,--version-script="$here/layer/exports.map" -s \
    "$here/layer/oal_capture_layer.cpp" -o "$out/lib/liboal_capture_layer.so"
$zigcc c++ $flags -s "$here/daemon/oal_frame_daemon.cpp" -o "$out/bin/oal-frame-daemon" -ldl
cp "$here/contrib/"* "$out/"
cp "$here/README.md" "$out/README.md"
(cd "$here/dist" && tar -czf oal-frame-aarch64.tar.gz oal-frame && sha256sum oal-frame-aarch64.tar.gz > oal-frame-aarch64.tar.gz.sha256)
rm -rf "$here/dist/.vkinc"
echo "Built $here/dist/oal-frame-aarch64.tar.gz"
