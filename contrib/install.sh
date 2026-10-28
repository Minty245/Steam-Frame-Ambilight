#!/bin/sh
# Installs OpenVR Ambient Light for Steam Frame into your home directory.
# Installs DISABLED: nothing changes in SteamVR until you run `oalctl enable`.
# No sudo, no changes under /opt or /usr. Undo with uninstall.sh.
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_dir="$HOME/.local/share/oal-frame"
config_dir="$HOME/.config/oal-frame"
unit_dir="$HOME/.config/systemd/user"
bin_dir="$HOME/.local/bin"
libexec_dir="$HOME/.local/libexec"

[ "$(id -u)" -ne 0 ] || { echo "Run as your normal user (steamos), not with sudo." >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "This build is for the Steam Frame (aarch64)." >&2; exit 1; }
for c in systemctl systemd-run install; do
    command -v "$c" >/dev/null 2>&1 || { echo "Missing command: $c" >&2; exit 1; }
done
[ -f "$src/lib/liboal_capture_layer.so" ] && [ -x "$src/bin/oal-frame-daemon" ] || {
    echo "Prebuilt files not found next to install.sh (lib/, bin/). Use the release archive." >&2; exit 1; }

was_enabled=false
[ -f "$data_dir/layer/oal_capture_layer.json" ] && was_enabled=true

install -Dm755 "$src/lib/liboal_capture_layer.so" "$data_dir/lib/liboal_capture_layer.so"
install -Dm755 "$src/bin/oal-frame-daemon" "$bin_dir/oal-frame-daemon"
install -Dm755 "$src/oalctl" "$bin_dir/oalctl"
install -Dm755 "$src/oal-guard" "$libexec_dir/oal-guard"
install -Dm644 "$src/oal-frame-daemon.service" "$unit_dir/oal-frame-daemon.service"
install -Dm644 "$src/91-oal-capture.conf" "$unit_dir/steamvr.service.d/91-oal-capture.conf"
install -d -m 700 "$config_dir"
install -d -m 755 "$data_dir/layer"
[ -f "$config_dir/oal.ini" ] || install -m 644 "$src/oal.ini" "$config_dir/oal.ini"

# Layer manifest. Installed as .disabled unless it was already enabled.
manifest_name="oal_capture_layer.json"
$was_enabled || manifest_name="oal_capture_layer.json.disabled"
rm -f "$data_dir/layer/oal_capture_layer.json.disabled"
cat > "$data_dir/layer/$manifest_name" <<EOF
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_OAL_capture",
    "type": "GLOBAL",
    "library_path": "$data_dir/lib/liboal_capture_layer.so",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "OpenVR Ambient Light capture for Steam Frame",
    "functions": {
      "vkGetInstanceProcAddr": "oal_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "oal_GetDeviceProcAddr"
    },
    "enable_environment": { "ENABLE_OAL_CAPTURE": "1" },
    "disable_environment": { "DISABLE_OAL_CAPTURE": "1" }
  }
}
EOF

# SteamVR environment. Our drop-in (91-) is read after FrameColor's (90-), so
# VK_ADD_IMPLICIT_LAYER_PATH must also carry FrameColor's layer folder or
# FrameColor would stop loading.
layer_path="$data_dir/layer"
fc_env="$HOME/.config/framecolor/vulkan.env"
if [ -f "$fc_env" ]; then
    fc_path=$(sed -n 's/^VK_ADD_IMPLICIT_LAYER_PATH=//p' "$fc_env" | tail -n 1)
    [ -n "$fc_path" ] && layer_path="$layer_path:$fc_path"
fi
# Keep capture settings you changed by hand.
prev_env="$config_dir/vulkan.env"
prev_hz=$(sed -n 's/^OAL_CAPTURE_HZ=//p' "$prev_env" 2>/dev/null | tail -n 1)
prev_grid=$(sed -n 's/^OAL_CAPTURE_GRID=//p' "$prev_env" 2>/dev/null | tail -n 1)
cat > "$config_dir/vulkan.env" <<EOF
VK_ADD_IMPLICIT_LAYER_PATH=$layer_path
ENABLE_OAL_CAPTURE=1
# Captures per second
OAL_CAPTURE_HZ=${prev_hz:-30}
# Capture detail: the largest grid (in cells) the panel image is shrunk to. 270 gives the Steam Frame
# a 270x135 grid (about 0.75% of an eye's width per cell). 128 gives 67x33. Restart SteamVR after changing.
OAL_CAPTURE_GRID=${prev_grid:-270}
EOF

systemctl --user daemon-reload
if $was_enabled; then
    systemctl --user restart steamvr.service
    systemctl --user restart oal-frame-daemon.service || true
    echo "Updated. The layer was already enabled, so SteamVR was restarted."
else
    echo "Installed (layer DISABLED, SteamVR untouched)."
    echo "Next:  $bin_dir/oalctl doctor   then   $bin_dir/oalctl enable"
fi
echo "Undo everything with: sh $src/uninstall.sh"
