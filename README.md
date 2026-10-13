# geneva-fp

Linux driver for the **Goodix GXFP5A8B** SPI fingerprint sensor found in **LG gram** laptops
(tested on the 15Z90N). Fingerprint login, lock screen and `sudo` through the standard
`fprintd` / PAM stack.

The sensor ("Geneva": GF3268 sensor + GM168SEC MCU) has no vendor Linux support. This project
is a clean-room driver built from reverse engineering; see [docs/PROTOCOL.md](docs/PROTOCOL.md).

- Native [libfprint](https://fprint.freedesktop.org/) **TOD** driver: loads into your
  distribution's own libfprint, no fork, no daemon.
- Matching with [SIGFM](https://github.com/goodix-fp-linux-dev/libfprint/tree/sigfm) (SIFT
  keypoints). The 108×88 sensor is too small for libfprint's minutiae matcher.
- `geneva-fp-tool` for setup and diagnostics.
- No proprietary files are shipped. The sensor configuration and firmware are extracted from
  LG's own Windows driver, which you download from LG.

## Supported hardware

| Laptop | Sensor (ACPI) | Firmware | Status |
|---|---|---|---|
| LG gram 15Z90N | `GXFP5A8B` | `GF3268_GM168SEC_APP_20011` | works |

Other LG gram models with a `GXFP5A8B` sensor probably work. Check with
`ls /sys/bus/acpi/devices/ | grep GXFP`. Please report results.

## Install

**Ubuntu 24.04 / 26.04.** Download the `.deb` for your release from
[Releases](../../releases), then run:

```sh
sudo apt install ./geneva-fp_*_amd64.deb
```

**From source** (Debian/Ubuntu):

```sh
git clone https://github.com/OWNER/geneva-fp && cd geneva-fp
./install.sh
```

## Setup (once)

1. **Get LG's driver.** Download LG's Windows "Fingerprint Driver" **3.1.100.320**
   (`GD_FP_W106_15_3.1.100.320.zip`) from LG's support page for your gram.
2. **Extract** the sensor configuration and firmware from it:
   ```sh
   sudo geneva-fp-tool extract GD_FP_W106_15_3.1.100.320.zip
   ```
3. **Pair the sensor with Linux.** Windows locks the sensor to itself with a random key.
   This step erases the sensor's application, writes a known key and reflashes the same
   firmware. It takes about 30 seconds. If it is interrupted, run it again.
   ```sh
   sudo geneva-fp-tool provision
   ```
4. **Enroll** a finger (12 touches), and enable fingerprint authentication:
   ```sh
   fprintd-enroll
   sudo pam-auth-update --enable geneva-fp   # 3 tries, then password
   ```

`sudo geneva-fp-tool status` shows the state of all of this at any time.

### Dual boot with Windows

Windows' LG driver re-pairs the sensor with Windows automatically. After you have used
fingerprint in Windows, Linux reports *"sensor is paired with another OS"*. Run
`sudo geneva-fp-tool provision` again; your enrolled fingers stay valid. Going back,
Windows re-pairs itself again on its own.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `fprintd-list` shows no device | `sudo geneva-fp-tool status`. If no spidev node is found, reboot once after installing. |
| "sensor configuration missing" | run `geneva-fp-tool extract` (setup step 2) |
| "paired with another OS" | run `geneva-fp-tool provision` (setup step 3) |
| frequent "try again" | press the whole pad flat, and enroll more placements of the same finger |
| `fprintd-verify` rejects your 2nd finger | expected: it checks only the first finger unless you pass `-f any`. PAM always uses all fingers. |

Debug logs: `sudo G_MESSAGES_DEBUG=all /usr/libexec/fprintd -t` in a terminal, after
`sudo systemctl stop fprintd`.

## Uninstall

`sudo apt remove geneva-fp`, or `./uninstall.sh` for a source install. Enrolled prints
live in fprintd (`fprintd-delete $USER`). To give the sensor back to Windows, just boot
Windows; its driver re-pairs it.

## How it works

```
fprintd ── libfprint (distro) ── TOD module libfprint-2-tod-geneva.so
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
