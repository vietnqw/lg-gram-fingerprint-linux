# Goodix GXFP5A8B ("Geneva") protocol notes

Reverse-engineered on an LG gram 15Z90N. The sources were LG's Windows driver (`Wbdi.dll`,
the `WbdiDataEnclave` SGX enclave) and the MCU firmware, with every claim below checked on
hardware. The code in `src/` is the reference implementation.

## Hardware

| | |
|---|---|
| ACPI device | `GXFP5A8B` (`spi-GXFP5A8B:00`) on the Intel LPSS SPI controller |
| Sensor / MCU | GF3268 (108 × 88 px, 12-bit) / GM168SEC, an **MSP430** MCU |
| Firmware | `GF3268_GM168SEC_APP_20011` (app at 0x4000), IAP bootloader `MILAN_GM168SEC_IAP_20003` (0xc00c) |
| SPI | mode 0, **chip select active-high**, works up to 10 MHz (we use 4 MHz) |
| Reset | `/dev/gpiochip0` line 46 (ACPI pad `GFPS`): high 10 ms, then low; the app is up ~0.4 s later |

## Transport

- **Write:** `F0 3D 00 <len BE16> <data>` in one transfer. **The data length must be even.**
  The MCU silently drops the last byte of an odd-length write. Pad with one byte and count
  it in the SPI length but not in the pack length. Windows does the same
  (`_WriteSpi`, "aligned to 2 bytes").
- **Read:** transfer `F0 3B 00`, then a second transfer starting with `F1` that clocks out
  the mailbox.
- **Stale mailbox:** the mailbox is not cleared by a reset. The previous session's last pack
  can still be there.

### Packs and messages

```
pack    : [flag][len LE16][sum(flag, len) & 0xff][body]
          flag A0 = plaintext message, B0 = TLS record(s), B2 = TLS application data
message : [cmd][len LE16 = payload + 1][payload][0xAA - sum(all previous bytes)]
```

Every command first gets an ACK message (`cmd B0`, payload `[acked cmd][status]`), then its
reply message with the same `cmd`.

| cmd | meaning | payload → reply |
|---|---|---|
| `20` | get image | `01 00` → image as a B2 pack (see below) |
| `32` / `34` | arm finger-down / finger-up detection | `0c 01` / `0e 01` + base → reply when it fires |
| `36` | finger detect, manual measure | `0d 01` + base → 4-byte header + 12 × u16 channel values |
| `90` | upload sensor config | 256 bytes → `01 00` |
| `a4` | erase app (drops into the IAP bootloader) | `00 32` |
| `a6` | read OTP | `00 00` → OTP bytes |
| `a8` | firmware version | `00 00` → ASCII string |
| `d0` | start a TLS session | `00 00` → ClientHello in a B0 pack |
| `d4` | "TLS established" | `00 00` → **the MCU's mbedTLS state** (0x10 = handshake over) |
| `e0` / `e4` | PSK write / read (PMK hash) | see Provisioning |
| `f0` / `f4` | firmware write / check (IAP) | |
| `ae` | (other Goodix MCUs: state query) | no reply on this firmware |

## Session

1. Reset, `a8` (expect `GF3268…`), `e4` (PMK hash must be ours; see Provisioning), `a6` OTP.
2. `d0`. The MCU is a **TLS 1.2 client** (mbedTLS) offering only
   `TLS_PSK_WITH_AES_128_CBC_SHA256` (0x00AE), identity `Client_identity`. The host is the
   server. Send ServerHello (no session id, no extensions) + ServerHelloDone as **one pack**.
   Partial records are fatal: the MCU's receive callback returns 0, which mbedTLS treats as
   EOF. The MCU answers with ClientKeyExchange, ChangeCipherSpec and Finished, one pack each.
   Reply with ChangeCipherSpec + Finished.
3. Poll `d4` until its reply is ≥ 0x10. Below that, `20` answers with a `d0` "reconnect"
   message instead of an image. A reply stuck at 0x0d means the MCU never accepted our
   Finished. In our case that was the odd-length write above eating the last padding byte.
4. `90` with the config. Every register 0x005c value is patched with the OTP Tcode
   `((otp[0x16] >> 4) + 1) << 4`, valid if `otp[0x16] + otp[0x17] == 0xff`. Then fix the
   checksum at offset 254: `-(0xA5A5 + Σ u16 words 0..126)`.

The PSK is 32 zero bytes. Templates never leave the host, and the TLS link only protects
images in transit on the SPI bus.

## Finger detection and images

- **Base:** `36` with base `80 00 × 12` measures 12 channel values `m`. The base is then
  `((m >> 1) << 8) | 0x80` per channel (LE). A finger lowers the values.
- **Waiting for a finger:** send `36` with the base (this also flushes stale replies), then
  `32` with the same base. Its reply arrives when a finger *arrives*; `34` replies when it
  leaves.
- **Image:** `20 01 00` → a B2 pack of 14334 bytes = 9-byte prefix `?? 20 len16 00 …` + one
  TLS application-data record. In a single long mailbox read the record starts about 1.4 KB
  in, so search for its `17 03 03` header.
- **Decoding:** the record decrypts to 14260 bytes = 108 × 88 12-bit pixels + a 4-byte
  trailer. Every 6 bytes hold 4 pixels:
  ```
  p0 = (b0 & 0xf) << 8 | b1     p1 = b3 << 4 | b0 >> 4
  p2 = (b5 & 0xf) << 8 | b2     p3 = b4 << 4 | b5 >> 4
  ```
- **Background:** frames carry a strong fixed row pattern. Subtract a frame taken without a
  finger. A real touch lowers the mean by 600–850 counts.

## Matching

NBIS finds only 3–9 minutiae per frame, so bozorth scores 0 even for identical frames. We use
SIGFM (SIFT + geometric consistency) on:
1. background-subtracted frames,
2. a 1st–99th percentile stretch,
3. a 2× bicubic upscale,
4. CLAHE (2.0, 4 × 4).

Over 8 enrolled touches, the same finger scored 289 to 7·10⁵ and a different finger 0–11.
The threshold is 100.

## Provisioning

Windows' SGX enclave writes a random PSK. To use a known one:
1. **Erase the app:** `a4 00 32`, then reset. The MCU is now in its IAP bootloader.
2. **Write the PSK:** `e0` with
   `[total LE32][len LE32][offset LE32] 56a5bb956b7c8d9e0000 [0xbb010003 LE32][96 LE32]
   <96-byte white-box PSK>`.
   The white-box blob encodes the all-zero PSK and is shared by Goodix MCUs
   (goodix-fp-dump).
3. **Check the PSK:** `e4` with `[32][0][0xbb020001][0]` returns `00 <flags> <len> <PMK hash>`.
   For the zero PSK the hash is `66687aad…5f2925`.
4. **Flash the app** in Windows' `WriteApp` format:
   `[crc32(size ‖ crc32(code))][size][crc32(code)][code]`, CRC-32/MPEG-2, in 256-byte `f0`
   chunks `[offset][len][2][data]`. Then reset.

The firmware and config are carved from `Wbdi.dll` (3.1.100.320, SHA-256 `7437c02c…`): the
config at file offset 0x11a6c0 (its stored checksum is wrong; recompute it) and the app image
at 0xf86fa (26456 bytes, of which the code is all but the last 4).

## Windows-side tooling that helped

- `Wbdi.dll` writes a verbose log when `HKLM\Software\Goodix\FP\LogOutput` `LogLevel` /
  `LogTarget` are set.
- The TLS server lives in the SGX enclave (stock mbedTLS); `Wbdi.dll` only relays records.
