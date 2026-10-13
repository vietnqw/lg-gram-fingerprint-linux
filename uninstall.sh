#!/bin/sh
# Remove a source install made with install.sh.
set -eu

cd "$(dirname "$0")"

sudo pam-auth-update --disable geneva-fp 2>/dev/null || true
if [ -d build ]; then
  sudo ninja -C build uninstall >/dev/null
else
  sudo rm -f /usr/bin/geneva-fp-tool \
    /usr/lib/*/libfprint-2/tod-1/libfprint-2-tod-geneva.so \
    /usr/lib/udev/rules.d/60-geneva-fp.rules \
    /usr/lib/modprobe.d/geneva-fp.conf \
    /usr/lib/systemd/system/fprintd.service.d/geneva-fp.conf \
    /usr/share/pam-configs/geneva-fp
fi
sudo pam-auth-update --package >/dev/null 2>&1 || true
sudo udevadm control --reload-rules
sudo systemctl daemon-reload
sudo systemctl try-restart fprintd.service || true

echo "Removed. Enrolled fingerprints (fprintd) and /var/lib/geneva-fp were kept;"
echo "delete them with:  sudo rm -rf /var/lib/geneva-fp && fprintd-delete \$USER"
