#!/bin/sh
# Removes everything install.sh added. Keeps ~/.config/oal-frame/oal.ini unless
# you pass --purge. Restarts SteamVR only if the layer was enabled.
set -u

data_dir="$HOME/.local/share/oal-frame"
config_dir="$HOME/.config/oal-frame"
unit_dir="$HOME/.config/systemd/user"
was_enabled=false
[ -f "$data_dir/layer/oal_capture_layer.json" ] && was_enabled=true

systemctl --user stop oal-probation.timer >/dev/null 2>&1 || true
systemctl --user disable --now oal-frame-daemon.service >/dev/null 2>&1 || true

rm -f "$unit_dir/steamvr.service.d/91-oal-capture.conf" "$unit_dir/oal-frame-daemon.service"
rmdir "$unit_dir/steamvr.service.d" 2>/dev/null || true
rm -f "$HOME/.local/bin/oal-frame-daemon" "$HOME/.local/bin/oalctl" "$HOME/.local/libexec/oal-guard"
rm -rf "$data_dir"
rm -f "$config_dir/vulkan.env" "$config_dir/layer-disabled" "$config_dir/events.log"
if [ "${1:-}" = "--purge" ]; then rm -f "$config_dir/oal.ini"; rm -rf "${XDG_STATE_HOME:-$HOME/.local/state}/oal-frame"; fi
rmdir "$config_dir" 2>/dev/null || true
rm -f /dev/shm/oal-capture

systemctl --user daemon-reload
if $was_enabled; then systemctl --user restart steamvr.service; fi
echo "OAL for Steam Frame removed.$( [ -f "$config_dir/oal.ini" ] && echo " Your settings were kept in $config_dir/oal.ini (use --purge to delete).")"
