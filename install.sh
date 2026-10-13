#!/bin/sh
# Build and install the GXFP5A8B fingerprint driver from source (Debian/Ubuntu).
#   ./install.sh [LG-DRIVER.zip]
# Run as a normal user; it uses sudo where needed.
set -eu

cd "$(dirname "$0")"
LG_DRIVER="${1:-}"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ -e /sys/bus/acpi/devices/GXFP5A8B:00 ] ||
  die "no Goodix GXFP5A8B sensor found on this machine (ACPI GXFP5A8B:00)"
command -v apt-get >/dev/null || die "this script supports Debian/Ubuntu (apt); see README for other distributions"

say "Installing build dependencies"
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config libglib2.0-dev libssl-dev libopencv-dev \
  libfprint-2-tod-dev fprintd libpam-fprintd unzip

say "Building"
[ -d build ] && RECONF=--reconfigure || RECONF=
meson setup build $RECONF --prefix=/usr --buildtype=release -Dtests=false >/dev/null
ninja -C build

say "Installing"
sudo meson install -C build --quiet

say "Activating (no reboot needed)"
sudo udevadm control --reload-rules
if [ -e /sys/module/spidev/parameters/bufsiz ] && [ "$(cat /sys/module/spidev/parameters/bufsiz)" -lt 65536 ]; then
  sudo modprobe -r spidev 2>/dev/null || echo "   (spidev is busy; the new buffer size applies after a reboot)"
fi
sudo udevadm trigger --action=add --subsystem-match=spi
sudo udevadm settle
sudo systemctl daemon-reload
sudo systemctl try-restart fprintd.service || true

if [ ! -s /var/lib/geneva-fp/firmware.bin ]; then
  if [ -z "$LG_DRIVER" ]; then
    say "Almost done: the sensor configuration comes from LG's Windows driver"
    echo "   Download LG's \"Fingerprint Driver\" 3.1.100.320 (GD_FP_W106_15_3.1.100.320.zip)"
    echo "   for your LG gram, then run:  ./install.sh /path/to/GD_FP_W106_15_3.1.100.320.zip"
    exit 0
  fi
  say "Extracting sensor configuration from $LG_DRIVER"
  sudo geneva-fp-tool extract "$LG_DRIVER"
fi

say "Checking the sensor"
if ! sudo geneva-fp-tool status; then
  echo
  say "The sensor must be paired with Linux once"
  sudo geneva-fp-tool provision
fi

say "Done. Next steps:"
echo "   fprintd-enroll                          # enroll a finger"
echo "   sudo pam-auth-update --enable geneva-fp # use it for login, lock screen and sudo"
