#!/bin/sh
# Build and install the GXFP5A8B fingerprint driver from source (Debian/Ubuntu).
#   ./install.sh [--driver FILE]   (FILE: optional offline Goodix/LG driver package)
# Run as a normal user; it uses sudo where needed.
set -eu

cd "$(dirname "$0")"
DRIVER=
[ "${1:-}" = "--driver" ] && DRIVER="${2:?--driver needs a file}"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ -e /sys/bus/acpi/devices/GXFP5A8B:00 ] ||
  die "no Goodix GXFP5A8B sensor found on this machine (ACPI GXFP5A8B:00)"
if ls /usr/local/lib/*/libfprint-2.so* /usr/local/lib/libfprint-2.so* >/dev/null 2>&1; then
  die "a libfprint build is installed in /usr/local and would shadow the system one; remove it first"
fi
command -v apt-get >/dev/null || die "this script supports Debian/Ubuntu (apt); see README for other distributions"

say "Installing build dependencies"
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config libglib2.0-dev libssl-dev libopencv-dev \
  libfprint-2-tod-dev fprintd libpam-fprintd curl gcab unzip

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

say "Setting up the sensor"
sudo lg-gram-fp-tool setup ${DRIVER:+--driver "$DRIVER"}
