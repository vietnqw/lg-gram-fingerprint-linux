# LG gram fingerprint for Linux

Fingerprint login, lock screen and `sudo` on Linux for the **2020 LG gram** (14Z90N, 15Z90N,
17Z90N), whose **Goodix GXFP5A8B** SPI fingerprint sensor has no vendor Linux support. It works
through the standard `fprintd` / PAM stack.

- Native [libfprint](https://fprint.freedesktop.org/) **TOD** driver: it loads into your
  distribution's own libfprint, with no fork and no daemon.
- Matching uses [SIGFM](https://github.com/goodix-fp-linux-dev/libfprint/tree/sigfm) (SIFT
  keypoints). The 108×88 sensor is too small for libfprint's minutiae matcher.
- `lg-gram-fp-tool` handles setup and diagnostics.
- No proprietary files are shipped. The sensor configuration and firmware are extracted from
  LG's own Windows driver, which you download from LG.
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
```

**From source** (Debian/Ubuntu):

```sh
git clone https://github.com/vietnqw/lg-gram-fingerprint-linux && cd lg-gram-fingerprint-linux
./install.sh
```

## Setup (once)

1. **Get LG's driver.** Download LG's Windows "Fingerprint Driver" **3.1.100.320**
   (`GD_FP_W106_15_3.1.100.320.zip`) from LG's support page for your gram.
2. **Extract** the sensor configuration and firmware from it:
   ```sh
   sudo lg-gram-fp-tool extract GD_FP_W106_15_3.1.100.320.zip
   ```
3. **Pair the sensor with Linux.** Windows locks the sensor to itself with a random key.
   This step erases the sensor's application, writes a known key and reflashes the same
   firmware. It takes about 30 seconds. If it is interrupted, run it again.
   ```sh
   sudo lg-gram-fp-tool provision
   ```
4. **Enroll** a finger (12 touches), and enable fingerprint authentication:
   ```sh
   fprintd-enroll
   sudo pam-auth-update --enable lg-gram-fingerprint   # 3 tries, then password
   ```

`sudo lg-gram-fp-tool status` shows the state of all of this at any time.

### Dual boot with Windows

Windows' LG driver re-pairs the sensor with Windows automatically. After you have used
fingerprint in Windows, Linux reports *"sensor is paired with another OS"*. Run
`sudo lg-gram-fp-tool provision` again; your enrolled fingers stay valid. Going back,
Windows re-pairs itself again on its own.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `fprintd-list` shows no device | `sudo lg-gram-fp-tool status`. If no spidev node is found, reboot once after installing. |
| "sensor configuration missing" | run `lg-gram-fp-tool extract` (setup step 2) |
| "paired with another OS" | run `lg-gram-fp-tool provision` (setup step 3) |
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

## Credits and license

LGPL-2.1-or-later, see [LICENSE](LICENSE) and [NOTICE](NOTICE).

- SIGFM by Matthieu Charette, Natasha England-Elbro and Timur Mangliev (LGPL-2.1+).
- The [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) community, whose
  goodix-fp-dump (MIT) documented the Goodix message format and white-box PSK.
- The GXFP5187/51A7 protocol notes by delitdesnoyers06-del.

Not affiliated with Goodix or LG. Use at your own risk; the provisioning step reflashes the
sensor's MCU with the vendor's own firmware.
