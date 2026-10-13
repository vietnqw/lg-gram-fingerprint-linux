# LG gram fingerprint for Linux

Fingerprint login, lock screen and `sudo` on Linux for the **2020 LG gram** (14Z90N, 15Z90N,
17Z90N), whose **Goodix GXFP5A8B** SPI fingerprint sensor has no vendor Linux support. It works
through the standard `fprintd` / PAM stack.

- Native [libfprint](https://fprint.freedesktop.org/) **TOD** driver: it loads into your
  distribution's own libfprint, with no fork and no daemon.
- Matching uses [SIGFM](https://github.com/goodix-fp-linux-dev/libfprint/tree/sigfm) (SIFT
  keypoints). The 108×88 sensor is too small for libfprint's minutiae matcher.
- `lg-gram-fp-tool` handles setup and diagnostics.
- No proprietary files are shipped. Setup downloads the vendor driver from Microsoft Update
  and keeps only the sensor's settings and firmware, on your machine.
- Clean-room, from reverse engineering. The protocol is documented in
  [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Is this for my laptop?

Run this:

```sh
ls /sys/bus/acpi/devices/ | grep GXFP5A8B
```

If it prints `GXFP5A8B:00`, this project is for you. Another sign is that LG's support page for
your model lists the fingerprint driver `GD_FP_W106_15_3.1.100.320`.

| LG gram | Fingerprint sensor | Status |
|---|---|---|
| **15Z90N** (2020) | Goodix GXFP5A8B (SPI) | ✅ works (developed and tested on it) |
| **17Z90N** (2020) | Goodix GXFP5A8B (SPI) | ✅ same sensor and driver package (please report) |
| **14Z90N**, **14T90N** (2020) | same LG driver package | 🟡 very likely the same sensor (please report) |
| 2021–2022 grams (e.g. 14Z90P, gram 14 2022) | Goodix USB `27c6:6a94` | ➡️ already supported by stock libfprint (`goodixmoc`), no need for this project |
| gram 17 (2025) | ELAN USB `04f3:0ca2` | ➡️ see [elan-fingerprint-linux](https://github.com/navycrow/elan-fingerprint-linux) |

Other laptops with `GXFP5A8B` should work too. Please open an issue with the output of
`sudo lg-gram-fp-tool status` either way.

## Install

**Ubuntu 24.04 / 26.04.** Download the `.deb` for your release from
[Releases](../../releases), then run:

```sh
sudo apt install ./lg-gram-fingerprint_*_amd64.deb
sudo lg-gram-fp-tool setup
```

Then enroll a finger in **Settings → Users → Fingerprint Login**, or with `fprintd-enroll`.
You're done.

**From source** (Debian/Ubuntu), one command builds, installs and runs the setup:

```sh
git clone https://github.com/vietnqw/lg-gram-fingerprint-linux && cd lg-gram-fingerprint-linux
./install.sh
```

### What `setup` does

It asks before each step:

1. **Downloads the sensor configuration.** The sensor needs a few settings that only exist inside
   the Goodix Windows driver, the one LG ships. They are copyrighted, so they are not included
   here. `setup` downloads that driver from Microsoft's official Windows Update server (1.5 MB),
   checks it, and keeps only the sensor settings and firmware. For offline use, pass
   `--driver GD_FP_W106_15_3.1.100.320.zip` with the file from LG's support site.
2. **Pairs the sensor with Linux.** Windows locks the sensor to itself with a random key. Setup
   gives it a new key and reflashes the sensor's own firmware. This takes about 30 seconds and
   is safe to re-run if interrupted.
3. **Turns on fingerprint for `sudo` and login**, if you want it (3 tries, then your
   password).

`sudo lg-gram-fp-tool status` shows the state at any time.

### Dual boot with Windows

If you use the fingerprint in Windows, Windows takes the sensor back automatically, and Linux
then reports *"sensor is paired with another OS"*. Run `sudo lg-gram-fp-tool setup` again;
your enrolled fingers stay valid.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `fprintd-list` shows no device | `sudo lg-gram-fp-tool status`. If no spidev node is found, reboot once after installing. |
| "sensor configuration missing" or "paired with another OS" | run `sudo lg-gram-fp-tool setup` |
| frequent "try again" | press the whole pad flat, and enroll more placements of the same finger |
| `fprintd-verify` rejects your 2nd finger | expected: it checks only the first finger unless you pass `-f any`. PAM always uses all fingers. |

Debug logs: `sudo G_MESSAGES_DEBUG=all /usr/libexec/fprintd -t` in a terminal, after
`sudo systemctl stop fprintd`.

## Uninstall

`sudo apt remove lg-gram-fingerprint`, or `./uninstall.sh` for a source install. Enrolled prints
live in fprintd (`fprintd-delete $USER`). To give the sensor back to Windows, just boot
Windows; its driver re-pairs it.

## How it works

```
fprintd ── libfprint (distro) ── TOD module libfprint-2-tod-lg-gram-fingerprint.so
                                   ├─ sensor.c   session: reset, PSK check, TLS, config, FDT, capture
                                   ├─ tls.c      TLS 1.2 PSK server (the MCU is the TLS client)
                                   ├─ device.c   spidev transfers + reset GPIO
                                   └─ match.cpp  preprocessing + SIGFM matching
```

The full protocol, including the details that made it hard, is in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

## Development

```sh
meson setup build && meson compile -C build && meson test -C build
sudo FP_TOD_DRIVERS_DIR=$PWD/build build/tests/fp-harness enroll /tmp/print   # driver without installing
```

To measure matching accuracy on your own fingers:

```sh
sudo build/lg-gram-fp-tool collect a 15    # 15 touches of one finger
sudo build/lg-gram-fp-tool collect b 8     # 8 touches of another finger
build/tests/match-eval a b                 # same-finger vs other-finger scores
```

Captured images are your fingerprints: keep them private and don't attach them to issues.

## Credits and license

LGPL-2.1-or-later, see [LICENSE](LICENSE) and [NOTICE](NOTICE).

- SIGFM by Matthieu Charette, Natasha England-Elbro and Timur Mangliev (LGPL-2.1+).
- The [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) community, whose
  goodix-fp-dump (MIT) documented the Goodix message format and white-box PSK.
- The GXFP5187/51A7 protocol notes by delitdesnoyers06-del.

Not affiliated with Goodix or LG. Use at your own risk; pairing reflashes the
sensor's MCU with the vendor's own firmware.
