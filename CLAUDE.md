# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is a Linux driver for the **Goodix GXFP5A8B** SPI fingerprint sensor in the 2020 LG gram (14Z90N, 15Z90N, 17Z90N). Goodix's codename for it is "Geneva": a GF3268 sensor plus a GM168SEC MCU (MSP430), running firmware `GF3268_GM168SEC_APP_20011`.

It is a **libfprint TOD module**: an out-of-tree driver that the distribution's own libfprint (`libfprint-2-tod1`) loads. It is not a libfprint fork. Matching is done inside the driver with SIGFM, because the 108×88 sensor is too small for libfprint's minutiae matcher (NBIS finds only 3–9 minutiae). `docs/PROTOCOL.md` describes the reverse-engineered protocol and is the reference for any protocol change.

## Commands

```sh
meson setup build                      # deps: glib, libcrypto>=3, opencv4, libfprint-2-tod-1
meson compile -C build
meson test -C build                    # all unit tests (proto, tls, match); no hardware needed
meson test -C build tls -v             # a single test suite
build/tests/test-proto -p /proto/fdt   # a single GTest case
dpkg-buildpackage -us -uc -b           # .deb (also runs the tests)
```

- Builds use `-Werror` (warning_level 2). The vendored `src/sigfm/` is compiled separately with warnings off.
- Meson options: `-Dtests=false` and `-Ddriver=false` (builds without the TOD headers).
- CI (`.github/workflows/ci.yml`) builds, tests and packages in Ubuntu 24.04 and 26.04 containers. Pushing a `v*` tag publishes a GitHub Release with both `.deb`s.

Hardware runs need root, the sensor bound to spidev, and the sensor config in `/var/lib/lg-gram-fingerprint/`:

- Normal setup: install the package or run `./install.sh`, then `sudo lg-gram-fp-tool setup`.
- Try a driver build without installing it: `sudo FP_TOD_DRIVERS_DIR=$PWD/build build/tests/fp-harness enroll|verify|identify FILE...`
- Measure matching accuracy: `sudo build/lg-gram-fp-tool collect DIR N` on two fingers, then `build/tests/match-eval DIR_A DIR_B`. These images are real fingerprints: never commit them.
- Hardware tests need a person to touch the sensor. Tell the user before starting a run that waits for a finger, and don't leave one running unattended.
- If the device fails to open with `EBUSY` on the reset GPIO, fprintd is holding the sensor. Stop it with `sudo systemctl stop fprintd`.

## Architecture

The layers, from the bottom up (all in `src/`). Each layer only uses the ones below it.

1. **`proto.c`**: pure framing, with no I/O. Pack `[flag][len][sum][body]` wraps message `[cmd][len][payload][0xAA - sum]`. Also image decoding (12-bit packed, 6 bytes → 4 pixels), the config checksum and Tcode patch, and the finger-detect (FDT) base.
2. **`tls.c`**: a hand-rolled **TLS 1.2 PSK server** (libcrypto). The MCU is the TLS *client* and supports only `PSK-AES128-CBC-SHA256`. The image arrives as one oversized application-data record that normal TLS libraries reject, which is why this is custom. The PSK is 32 zero bytes.
3. **`device.c`**: spidev (mode 0, **CS active-high**, 4 MHz) and the reset GPIO through the GPIO v2 uAPI. **Writes must be padded to an even length**, otherwise the MCU drops the last byte; this was the hardest bug in the project.
4. **`sensor.c`**: the blocking, cancellable session: reset → firmware check → 0xE4 PSK-hash check → TLS handshake → poll 0xD4 until the MCU's mbedTLS state is ≥ 0x10 → upload config. Also the background frame, FDT finger-down/up waits, and capture with signal guards. A failed operation invalidates the session and is retried once. Cancellation keeps the session.
5. **`match.cpp`**: preprocessing (background − frame, percentile stretch, 2× upscale, CLAHE), then SIGFM features and score. `GV_MATCH_THRESHOLD` = 100. Templates are serialized in a compact `GVF1` u8 format.
6. **`tod-geneva.c`**: the `FpDevice` driver. Each capture or lift-wait runs as one `GTask` in a worker thread on the blocking `sensor.c` API; results come back on the main loop.
   - Enroll, verify and identify do matching here. Prints carry `fpi-data` = `(q aay)` = (version, serialized features).
   - Suspend is detected through the `CLOCK_BOOTTIME`/`CLOCK_MONOTONIC` gap.
   - A finger left on the sensor from the previous attempt is waited out before the next capture.
   - The driver id `geneva` is part of fprintd's print storage path. Changing it orphans users' enrolled prints.
7. **`provision.c`** and **`tools/lg-gram-fp-tool.c`**: the setup CLI.
   - Downloads Goodix driver 3.1.100.320 from Microsoft Update (SHA-256 pinned), unpacks it with `gcab` (or reads LG's `.zip` / `Wbdi.dll`), and carves the 256-byte config and 26 KB firmware at fixed offsets, each hash-checked.
   - "Pairing" erases the MCU app, writes the white-box-encoded zero PSK, and reflashes the firmware in the vendor's WriteApp format. It is destructive but safe to re-run.

`hw.h` holds the platform constants: ACPI `GXFP5A8B`, `/dev/gpiochip0` line 46, data paths. `data/` and `debian/` provide the system integration:

- a udev rule that binds the device to spidev;
- `spidev bufsiz=65536` (image reads are about 18 KB);
- an fprintd drop-in that allows `/dev/gpiochip0` (fprintd's sandbox blocks it otherwise);
- a PAM profile, disabled by default.

## Constraints

- **No proprietary files in the repo.** The sensor config and firmware are LG/Goodix copyrighted, so they are only downloaded or extracted on the user's machine. The only vendor-derived constants that may be committed are the white-box PSK and the hashes and offsets.
- `src/sigfm/` is vendored LGPL-2.1+ code. Keep its notices, and record any modification in its header and in `NOTICE`.
- Commits are authored as `Viet Ngo <vietnq.work@gmail.com>`.
