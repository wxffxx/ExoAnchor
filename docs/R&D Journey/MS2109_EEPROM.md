# 2026-08-23 / 2026-10-29 MS2109 AT24C16 EEPROM (U5): write-protect control, external-programmer write and read-only backup through the P4

English mirror of [MS2109_EEPROM_zh.md](MS2109_EEPROM_zh.md). The
Chinese version is authoritative.

- Time: phase 1 on 2026-08-23 16:12–18:40; phase 2 on 2026-10-29 10:4x (both Beijing time)
- Hardware: two ExoAnchor PrototypeV2.4 boards (ESP32-P4 rev3.2, MS2109 + AT24C16 U5), called
  V2.4 board A (2026-08-23) and V2.4 board B (2026-10-29) below; an external CH341 programmer
  (VID:PID `1A86:5512`)
- Firmware: board A was flashed with `0.87.6-dev` r35, r36 and r37 in turn (ESP-IDF 5.5.5); board B
  was temporarily flashed with `0.88.0-v2.4-ms-test` and restored to its original `0.88.0-dev`
  afterwards
- Image: `MS2109_24C16_Demo_V1.8.1` from the vendor demo package (2048 bytes, 0xFF-padded), SHA-256
  `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715`; the image itself is not
  published with this report
- The read-out of board B through the P4 is also covered in section 3.2 of
  `P4_IN_SYSTEM_PROGRAMMING_zh.md`

## 1. Summary

| Item | Board | Result |
| --- | --- | --- |
| GPIO16 write-protect control (`eeprom-wp status/write-enable/protect`) | A | Verified on r37: first read, open-drain pull-low, open-drain release and the on-board pull-up all have on-board results |
| r36 `eeprom-wp status` misreports GPIO16 as low | A | Root cause found, fixed in r37; first read after the fix is `level=1 protected` |
| Writing Demo V1.8.1 to a blank U5 with an external CH341 | A | Verified: two pre-reads identical (all 0xFF); in-tool verify, independent read-back and the read after restoring write protection all match the locked image |
| First power-on after the write | A | Whole-board power-on boot, both MS2109 rails on, UVC enumerated 55 modes; the CH341 was still attached, so this is not an independent cold boot with the fixture removed |
| Actual MJPEG frames and KVM picture after the write | A | Not verified |
| MS2109 actually loading the new EEPROM | A | Not verified |
| Probing the 8 block addresses (0x50–0x57) through the P4 | B | Verified, `address_mask=0xff` |
| Reading all 2048 bytes through the P4 | B | Verified: two dumps byte-identical, SHA-256 equal to the locked Demo V1.8.1 image |
| Original firmware restored, NVS unchanged | B | Verified |
| Writing U5 through the P4 | — | Never tested |
| `ms2109-eeprom-tool.py dump` | B | Failed (serial sync problem); tool not fixed |
| Cause of the all-0x00 CH341 read during board A's early preflight | A | Not found |

U5 on both boards now holds Demo V1.8.1. To be explicit: **U5 has only ever been written with an
external CH341 programmer (board A); writing U5 through the P4 has never been tested.** The P4 path
has so far only been verified for reading (board B).

## 2. Method and safety design

### 2.1 Connections

Per the V2.4A4 netlist:

| Function | Part / net | ESP32-P4 |
| --- | --- | --- |
| MS2109 main power switch | U7 TPS22918, ON active high | GPIO13 |
| MS2109 1.2V enable | U10 TLV62569, EN active high | GPIO18 |
| AT24C16 write protect | U5 WP, high = protected, 4.7k on-board pull-up | GPIO16 |
| AT24C16 clock | MS_SCL, through R31 0Ω | GPIO47 |
| AT24C16 data | MS_SDA, through R61 0Ω | GPIO48 |

U5 is powered from `3V3_SYS` and stays powered when the MS2109 is switched off. The P4 can therefore
switch the MS2109 off first and then own the I²C bus; it must not drive this shared bus while the
MS2109 is powered.

### 2.2 Write-protect commands in the product firmware (from r36)

r36 decoupled the `eeprom-wp` serial command from the destructive MS2109 test features. The normal
V2.4 product configuration still contains no EEPROM read/write driver, no EEPROM HTTP interface and
no EEPROM emulator; the ELF was checked to contain none of these symbols and no
`ms-eeprom program-b64`.

- `eeprom-wp status`: reads the current WP input level only;
- `eeprom-wp write-enable`: pulls WP low with an open-drain output, held until `protect` or a reboot;
- `eeprom-wp protect`: releases the open-drain output so the on-board pull-up restores write
  protection.

`write-enable` and `protect` read the physical level back right after configuring the GPIO and
return `ESP_ERR_INVALID_RESPONSE` if it does not match the target; a successful GPIO API return is
not taken as evidence that WP has switched.

### 2.3 External CH341 write transaction (2026-08-23, board A)

- Every P4 write was preceded by an identity gate (serial number, chip revision, eFuse MAC, flash
  part and size, target profile, port not in use); no `erase-all`, no writes to NVS, TF or the OTA
  slots. The other V2.4 (board B) was neither reset nor accessed that day.
- U5 was handled as a single-chip transaction. All of the following were mandatory gates: locked
  SHA-256 of the image and the tools; two pre-reads that must agree; in-tool verify; independent
  read-back by the batch tool; one more read after restoring write protection.
- Authorization to overwrite non-empty content had been obtained beforehand, but U5 was blank before
  the write, so no non-empty content was overwritten.

Locked items:

| Item | SHA-256 |
| --- | --- |
| Image `MS2109_24C16_Demo_V1.8.1_CH341_2KiB_FFpad.bin` (2048 bytes) | `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715` |
| CH341 tool | `62b54b1fe1a83bd61b3e17d103d910e5940934db612dfb4a210a3684e7b9c2bc` |
| Batch tool v1.1.0 | `32b1496cd28dc806cec1d83a669af871f51ae91dddc19bf0a9bfd926405c043e` |

### 2.4 Safe transaction in the ms-test firmware (2026-10-29, board B)

`exoanchor-prototype-v2.4-ms-test` is a separate hardware-validation firmware, not part of the
product firmware. Every U5 read or write follows this order, and the failure path runs the same
cleanup:

1. Record the MS2109 power state, switch off the 1.2V of U10 first, then the `3V3_MS` of U7;
2. Wait for the rails and the shared bus to settle;
3. The P4 takes GPIO47/48 as I²C master;
4. GPIO16 is pulled low only for a write; reads always keep write protection on;
5. After a write, read back the whole chip and compare;
6. Release GPIO16 so the 4.7k on-board pull-up restores write protection;
7. Delete the I²C master and release GPIO47/48;
8. If the MS2109 was powered before the transaction, restore `3V3_MS` first, then the 1.2V.

The write command `ms-eeprom program-b64` requires a complete 2048-byte image and a trailing
`CONFIRM`; there is no chip erase, no write at an arbitrary offset and no unconfirmed write. By
design it writes 16-byte pages, waits for the write cycle after each page, reads the whole chip back
on the device, and the host reads it once more independently and compares CRC32 and every byte; the
firmware neither embeds nor auto-selects any MS2109 image. This write path has not been run on a
board to date.

The scope this time was read-only: no writes to the EEPROM, NVS or partition table, and the original
firmware restored afterwards. Before starting, the board was checked to have no running ExoAnchor
session and the serial port to be free.

## 3. Experiments and findings

### 3.1 Phase 1 (2026-08-23, board A)

**r35: first flash of the new board (16:12–16:23)**

- Identity gate passed: ESP32-P4 v3.2, 16 MiB SPI flash (manufacturer `c8` / device `4018`), target
  profile `exoanchor-prototype-v2.4`.
- Full 16 MiB backup before flashing (16777216 bytes), SHA-256
  `dffab0dd410657cb30c7b2fd7f2586a4792e8472e58882b3532581f8111a646d`, every byte 0xFF; no factory
  content to keep.
- r35 written (app 3070224 bytes, SHA-256 `e0eaa16a…f0cf`), DIO, 80 MHz, 16 MiB. The in-tool hash
  checks of all four segments and an independent `verify_flash` all passed.
- Normal boot: 32 MiB PSRAM at 250 MHz, Ethernet 100 Mbps full duplex, root page HTTP 200. There was
  no TF card on site and SDMMC init ended with `ESP_ERR_TIMEOUT`. KVM, MS2109 video, HID and other
  functions were not tested.

**r36: write-protect commands (17:21–18:18)**

- Host tests and the official V2.4 Dev build passed, app 3070992 bytes (SHA-256 `c2742f60…0995`).
  After flashing, the four in-tool hash checks and an independent `verify_flash` passed.
- The first on-board `eeprom-wp status`, with `write-enable` never having been run, reported
  `gpio=16 level=0 write=enabled result=ESP_OK` (see 4.1). Only the safety-increasing
  `eeprom-wp protect` was run afterwards; it returned
  `gpio=16 level=1 write=protected result=ESP_OK`, showing that the open-drain release and the
  on-board pull-up work.
- r36 was withdrawn at once and was never used to unlock or write U5.
- In the same period the external CH341's preflight double read of U5 returned 2048 bytes of 0x00
  (see 4.2).

**r37: fix and write-protect loop (18:18–18:40)**

- The `status` path now calls `gpio_input_enable()` first, without changing the existing open-drain
  output state. Host tests and the official build passed; app 3070992 bytes, SHA-256
  `30d903fe53448242a7e4adc403862e277646e25c5c920968015b5033f3f01be1`.
- Chip revision and eFuse MAC were confirmed again before writing. DIO, 80 MHz, 16 MiB, 460800 baud;
  the four in-tool hash checks and an independent `verify_flash` all matched.

| Stage | Serial result |
| --- | --- |
| r37 first `status` | `gpio=16 level=1 write=protected result=ESP_OK` |
| `write-enable` before the write | `gpio=16 level=0 write=enabled result=ESP_OK` |
| `protect` after the write | `gpio=16 level=1 write=protected result=ESP_OK` |
| Final `status` | `gpio=16 level=1 write=protected result=ESP_OK` |

**Writing U5 with the external CH341**

1. The two pre-reads were identical, both all 0xFF, SHA-256
   `d0ff1b294b5288d1ae1421eadf5b2d38a8752b76d472ff30bed9028e25b1c5b8`.
2. The locked image was written at 100 kHz as 256 pages of 8 bytes; the in-tool 2048-byte verify
   passed.
3. The batch tool's independent read-back `post-read.bin` matched the target byte for byte, with the
   locked SHA-256.
4. After `protect` restored write protection, `post-protected-read.bin` was read and again matched
   the target byte for byte, with the locked SHA-256.

The transaction result was PASS. The online state seen after the write (version, Ethernet, HTTP 200)
came from a boot before U5 was written and is not evidence that the new EEPROM was loaded.

**First power-on after the write**

- The device came up with a new boot, and the auth-status interface reported
  `reset_reason=power-on`, showing that the whole board had been power-cycled.
- Serial confirmed: r37 running normally; GPIO16 still `level=1 / protected / ESP_OK`; the MS2109
  GPIO13 3.3 V switch and GPIO18 1.2 V enable both on, order `3v3-first`, result `ESP_OK`.
- UVC init succeeded and enumerated 55 modes, active 1920×1080 MJPEG, target 1920×1080@25. There was
  no capture owner at the time, so streaming/capture was 0.
- The external CH341 was still enumerated on USB, however, so this boot does not count as "an
  independent cold boot with the programming fixture removed". The KVM page stopped at the local
  account login; no credentials were read or filled in, so no video owner was established and no
  frames were obtained.
- `video-h264-test`, which needs no login, exited before capture started for lack of contiguous
  internal memory (see 4.3), so it obtained no frames either.

Per the checkpoint, signing off that the MS2109 loads the new EEPROM requires unplugging the CH341
(SCL/SDA first, GND last), removing all power from the board for at least 5 seconds and confirming
that the MS2109 3.3 V / 1.2 V rails drop, powering up again, and then having the operator log into
the KVM page to accept actual MJPEG frames and the picture.

### 3.2 Phase 2 (2026-10-29, board B): read-only backup through the P4

**Test firmware**: `./tools/build-firmware.sh --profile stable --board exoanchor-prototype-v2.4-ms-test
--build-dir build-v24-mstest`, version `0.88.0-v2.4-ms-test`, app
`a60a914781139e5e5de5e8cbb40d9124482221782d5a921176ae00caf2fa83fb` (2155056 bytes). The first build
failed; see 4.5.

**Procedure**:

1. Read-only NVS snapshot: 0x9000, 24 KiB, SHA-256
   `02653103192f54bcdd9fbeb56543b70bc054996960374ed99a794c6fdaf8f684`.
2. After the identity check (ESP32-P4 v3.2, MAC matching the registered value) passed, bootloader,
   partition table, otadata, phy and app were backed up per segment (original app
   `a38d752279d1aadb…`, 0.88.0-dev, 4 MiB partition). The ms-test partition table matches the one on
   the board and was not rewritten.
3. Flashing ms-test: only bootloader and app written, `verify_flash` matched, then reset. The boot
   log contains `V2.4 MS test ready 3v3=GPIO13 1v2=GPIO18 wp=GPIO16 scl=GPIO47 sda=GPIO48`.
4. `ms-switch status`: `switch_level=1 core_level=1 command=on sequence=3v3-first last=ESP_OK`.
5. `ms-eeprom status`: `type=AT24C16 size=2048 page=16 wp=protected scl_level=1 sda_level=1
   address_mask=0xff probe=ESP_OK`; all 8 block addresses answered.
6. `ms-eeprom dump-b64` twice, both returning `ESP_OK`, byte-identical: SHA-256
   `2dd56419dedd50ab843b4cd1a0282c9e4e8e849d90a9481bf1c65399680b4715`, CRC32 `3e1888f5`. Nothing was
   written to the EEPROM during the read-out and write protection stayed on throughout.

**Image content**:

| Offset | Content | Meaning |
| --- | --- | --- |
| 0x000 | `A5 5A` | MS2109 EEPROM valid marker |
| 0x002 | `02 AE` | Patch code length 0x2AE = 686 bytes |
| 0x004 | `01 00` | Configuration bytes |
| 0x00C | `20 22 12 28` | BCD date 2022-12-28 |
| from 0x030 | `02 CC 03 …` | 8051 code, first instruction LJMP 0xCC03, load address 0xCC00 |
| after 0x2E1 | all 0xFF | Unused |

- 695 non-0xFF bytes in total, in 8 runs. There are no ASCII or UTF-16 strings, so no custom USB
  descriptors are visible.
- It differs from all five reference images in the vendor material (JZY-HDMI-V1.4, L-331-R1.2,
  SFX_VCMS_0101_V2, noname_1, OZC4).
- A comparison later that day found that the SHA-256 is identical to the locked image written to
  board A in phase 1, i.e. U5 on board B also holds `MS2109_24C16_Demo_V1.8.1` from the vendor demo
  package.

**Restore and verification**: `restore_board.py --write` was run from a copy of the backup, writing
back bootloader, otadata, phy and app; `verify_flash` matched. After boot the board ran `0.88.0-dev`,
`exoanchor-prototype-v2.4`, and the identity check passed; NVS read again was still `02653103…`; the
HTTPS root returned 200; both MS2109 rails came up and UVC enumerated 1920×1080 and other MJPEG modes
again.

## 4. Problems

### 4.1 r36 `eeprom-wp status` falsely reported write protection as off

Symptom: `write-enable` had never been run, yet the first `status` reported `level=0 write=enabled`.

Cause: after reset the GPIO16 input path is not yet enabled, so the value from a direct
`gpio_get_level()` is not valid evidence of the state.

Handling: r36 was withdrawn at once and never touched U5; r37 calls `gpio_input_enable()` first in
the `status` path, without changing the existing open-drain output state. The first `status` on r37
was `level=1 protected`.

### 4.2 CH341 read all 0x00 during the early preflight

During the r36 stage the external CH341's preflight double read of U5 returned 2048 bytes of 0x00.
At the time this was judged to be possibly real content or possibly a wiring or bus fault, and not
grounds to treat the EEPROM as writable. The formal transaction did not reproduce it; the formal
double read was consistently all 0xFF. The early files are kept only to trace the anomaly and cannot
replace the formal pre-write backup. The cause was not found.

### 4.3 `video-h264-test` memory precondition failed after the write

At board A's first power-on after the write, `video-h264-test` exited before capture started:
`internal=35955 largest=15360 need=24576/18432`, result `ESP_ERR_INVALID_STATE`. It neither accessed
nor modified U5 and obtained no frames; it was a runtime memory precondition failure at that moment
and is not evidence of an MS2109/UVC image failure.

### 4.4 `ms2109-eeprom-tool.py dump` failed

Symptom: the tool's `dump` reported "device did not return a complete verified EEPROM dump" twice,
while `status` and `power status` of the same tool succeeded.

Cause: the raw serial data showed two things: board B's CH343 resets the P4 whenever the port is
opened, so the command was sent while the board was still booting; and after boot a stray byte was
left in the input buffer, turning the command into "?ms-eeprom dump-b64", which was answered with
`unknown command`.

Handling: a temporary script was used instead: open the port, wait for "CLI ready", send an empty
line to flush the buffer, then send the command. The two dumps in step 6 of 3.2 were taken this way.
The tool itself has not been changed yet.

### 4.5 First ms-test build failed

The `h264-stages` serial command added in 16fb7c20 was not conditionally compiled on H.264, and the
Stable composition lacks the matching type. Fixed and committed as 8d3e2505, after which the build
passed.

### 4.6 Restore tool overwrites logs

`restore_board.py` overwrites the logs in the backup directory it runs from, so the original firmware
backup was copied separately (with SHA256SUMS) and the restore was run from the copy.

## 5. Open issues

- Writing U5 through the P4 has never been tested. Before it is used: user authorization and a
  2048-byte image to write; fix the tool's serial sync first (wait for "CLI ready", send an empty line,
  then the command); back up again before writing; first write the same image that was read out back
  unchanged to validate the write path, and only then consider writing new content.
- The serial sync problem in `ms2109-eeprom-tool.py` is not fixed.
- Board A: an independent cold boot with the CH341 removed, acceptance of actual MJPEG frames and the
  KVM picture, and confirmation that the MS2109 loads the new EEPROM are all still outstanding.
- Board A: the cause of the all-0x00 early preflight read was not found.
- Board A: TF, HID, Locator, PWR/RST, detect inputs and broader Agent/KVM functions were not verified
  in this phase.
