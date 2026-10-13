#!/bin/sh
# Remove a source install made with install.sh.
set -eu

cd "$(dirname "$0")"

sudo pam-auth-update --disable lg-gram-fingerprint 2>/dev/null || true
if [ -d build ]; then
  sudo ninja -C build uninstall >/dev/null
else
  sudo rm -f /usr/bin/lg-gram-fp-tool \
    /usr/lib/*/libfprint-2/tod-1/libfprint-2-tod-lg-gram-fingerprint.so \
    /usr/lib/udev/rules.d/60-lg-gram-fingerprint.rules \
    /usr/lib/modprobe.d/lg-gram-fingerprint.conf \
    /usr/lib/systemd/system/fprintd.service.d/lg-gram-fingerprint.conf \
    /usr/share/pam-configs/lg-gram-fingerprint
fi
sudo pam-auth-update --package >/dev/null 2>&1 || true
sudo udevadm control --reload-rules
sudo systemctl daemon-reload
sudo systemctl try-restart fprintd.service || true

echo "Removed. Enrolled fingerprints (fprintd) and /var/lib/lg-gram-fingerprint were kept;"
echo "delete them with:  sudo rm -rf /var/lib/lg-gram-fingerprint && fprintd-delete \$USER"
