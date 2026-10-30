# 2026-10-28 Restarting the MS2109 reboots the whole board: investigation and fix

English mirror of [MS2109_RESTART_REBOOT_zh.md](MS2109_RESTART_REBOOT_zh.md). The
Chinese version is authoritative.

- Date: 2026-10-28
- Firmware: `0.88.0-dev AzureDanube`, bench no-auth build
  (`build-v24-dev-088-noauth`), ESP-IDF 5.5.5
- Hardware: ExoAnchor PrototypeV2.4 (ESP32-P4 rev3.2, 32 MB PSRAM at 250 MHz),
  MS2109 USB capture, with its 3.3 V / 1.2 V rails under firmware control
  (GPIO13 / GPIO18)
- Fix: [`CMakeLists.txt`](../../device/ESP32P4/firmware/v0.86.6-dev/CMakeLists.txt) and
  [`main/drivers/video_input.c`](../../device/ESP32P4/firmware/v0.86.6-dev/main/drivers/video_input.c);
  verified on the board, **not committed yet**

## 1. Summary

After restarting the MS2109 from the KVM page, the page showed UNKNOWN for a
while. The cause was not a slow endpoint: the whole board rebooted. When the
MS2109 powered back up, USB enumeration failed for lack of internal DMA memory;
about 5 s later the firmware's recovery cut USB root-port power, and the ESP-IDF
hub driver, holding no device node, treated the device as still present and hit
the assertion in `hub.c:435`.

| Metric | Before | After |
| --- | --- | --- |
| Whole-board reboot after an MS2109 power off/on | 3 of 5 | 0 of 6 |
| `/api/status` failures during the cycles (0.5 s polling, 6 s timeout) | 8 | 0 |
| MS2109 power-on to frames flowing again | — | 1.7–1.9 s |
| USB out-of-memory errors on serial (`HCD Pipe alloc` / `EP Alloc` `ESP_ERR_NO_MEM`) | before every reboot | 0 |
| 1080p25 H.264 (standalone client) | 24.3 fps (same-day build without the descriptor move) | 24.17 fps, 0 source drops, 0 backpressure drops |

This is the open issue listed in section 7 of the
[0.88 experiment log](VIDEO_STREAMING_0.88.md): "reopening UVC reports
`EP Alloc error: ESP_ERR_NO_MEM` for 1–10 s" after a capture-card power cycle.
Both the `EP Alloc` and the `HCD Pipe alloc` errors come from the internal-DMA
allocation of transfer-descriptor lists inside IDF `hcd_pipe_alloc()`, not from
the usb_host_uvc stopped-URB-ring patch suspected at the time; this fix leaves
that patch untouched and the errors are gone.

## 2. Method

**Reproduction**: switch the MS2109 off through `/api/ms2109/power` and back on
4 s later, while polling `/api/status` every 0.5 s (6 s timeout) and recording
failures and any drop in `server_uptime` (a drop means a reboot).

**Serial**: the USB serial log (115200) was captured throughout, and crash
addresses were resolved with addr2line.

**Verification after the fix**: each cycle is 4 s off, then on, then a 50 s
wait. "Frames flowing again" is the time from power-on until
`video.frames_captured` in `/api/status` grows again. Meanwhile the bench
`h264_measure.py` pulls H.264 at 1920×1080@25 in back-to-back 40 s sessions
(touching the lease every 3 s), standing in for a browser that watches and
reconnects. Before each cycle test, 1080p25 throughput is measured on its own.

**Diagnostics**: the re-plug path logs the free size and largest block of the
internal DMA heap (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`); a temporary
`heap_caps_register_failed_alloc_callback` printed failed DMA allocations (removed
from the final build).

**Caveat**: a leftover background serial capture holding the port made esptool
fail at "Connecting" with
`device reports readiness to read but returned no data`, and the board then
dropped off the network until it was reflashed. Before every flash, stop any
capture and confirm with `lsof` that the port is free.

## 3. Problems and root causes

In the order they were found.

### 3.1 UNKNOWN was a whole-board reboot

Symptom: in the first reproduction the board vanished from the network about
5.5 s after the MS2109 powered on; when it came back, `server_uptime` was only
19 s. Over 5 consecutive off/on cycles the board rebooted 3 times, with uptime
restarting from 10–12 s each time.

After each reboot the Agent replays `TASKS.LOG` (2480 records, 1666831 B), which
took 45188–58394 ms. During that time the `si_agent_recove` task held CPU 0,
`task_wdt` reported that IDLE0 had not fed the watchdog, and many HTTPS
handshakes failed (`-0x7780`). The page could not get status during this window
either, which is why UNKNOWN lasted longer.

### 3.2 The crash: the assertion in `hub.c:435`

The serial log shows the same sequence before every reboot (one instance):

```
E (22630) USBH: HCD Pipe alloc error: ESP_ERR_NO_MEM
E (22630) HUB: Failed to add new device
W (27212) HUB: Device tree node (parent_port=0): not found
ESP_ERROR_CHECK failed: esp_err_t 0x105 (ESP_ERR_NOT_FOUND)
file: "/IDF/components/usb/hub.c" line 435
expression: dev_tree_node_dev_gone(NULL, 0)
```

Root cause: after the MS2109 powers up, EP0 pipe allocation fails and the hub
creates no device node. About 5 s later
(`SI_UVC_MISSING_DEVICE_RECOVERY_DELAY_MS` = 5000), the "missing device" recovery
in `video_input.c` calls `usb_host_lib_set_root_port_power(false)` and the hub's
root port enters `NOT_POWERED`. IDF 5.5.5 treats any disconnection event in that
state as having a device on the port and calls `dev_tree_node_dev_gone(NULL, 0)`,
which finds no node, returns `ESP_ERR_NOT_FOUND`, and `ESP_ERROR_CHECK` turns
that into an abort.

Fix: `power_cycle_uvc_root_port()` first checks with
`usb_host_device_addr_list_fill()` whether the hub holds a device. If it does
not, it no longer cuts root-port power; when the MS2109 is powered and no cold
start is running, it calls `si_ms2109_power_cycle(1000)` to re-plug the capture
card instead, which the hub sees as an ordinary unplug and replug.

With this change, 6 cycles caused no reboot and no `/api/status` failure. But
video did not come back: the serial log showed `open retained UVC` once a second,
each followed by `EP Alloc error: ESP_ERR_NO_MEM`, and later enumerations still
reported `HCD Pipe alloc error`.

### 3.3 No automatic retry after a failed enumeration

Root cause: the hub does not retry a failed attach by itself, and because the
supervisor keeps the previous capture selection it stays in the "UVC open
failed" branch and never reaches the "missing device" recovery.

Fix: the open-failure branch records when "no enumerated device" started, and
after 5 s of that (and at least 15 s since the last recovery) calls the same
re-plug function.

Result: 7 cycles without a reboot, but video still did not come back;
`last_error` was `open UVC stream: ESP_ERR_NO_MEM` or `ESP_ERR_NOT_FOUND`. The
serial log showed the re-plug firing
(`no enumerated USB device; power-cycling MS2109`), and the enumeration after
each re-plug failed with `NO_MEM` again.

### 3.4 The `NO_MEM` is not transient: a fragmented internal DMA heap

With internal DMA heap figures added to the re-plug log: without a viewer,
enumeration succeeded; with a 1080p25 H.264 viewer, free memory at the moment of
failure was 3359–6999 B and the largest block only 624–2016 B.

IDF `hcd_pipe_alloc()` allocates 2 buffer blocks per pipe, each with a
transfer-descriptor list obtained by `heap_caps_aligned_calloc(512, ...)` with
caps `MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL`. A control
pipe uses 3 descriptors (24 B), an isochronous pipe 64 (512 B). Because of the
512-byte alignment, TLSF needs a contiguous free block of about 1 KiB (EP0) /
about 1.5 KiB (isochronous). The failed-allocation callback confirmed it: the
failing request was 24 B with caps `0x80808`, while the largest block was
624 / 624 / 1440 B. The sdkconfig sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`,
and the internal heap is fragmented while a viewer is active.

### 3.5 Fix: move only the descriptor lists to PSRAM

IDF's Kconfig states that the ESP32-P4 USB-DWC DMA can access external RAM.
`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` moves both the descriptor lists
(`XFER_DESC_LIST_CAPS` in `hcd_dwc.c`) and the URB data buffers
(`DATA_BUFFER_CAPS` in `usb_private.c`), and the latter slows ingest (see
section 4).

Fix: `CMakeLists.txt` follows the project's existing esp_http_client patch. It
checks the SHA256 of IDF `usb/hcd_dwc.c` (`33fea905…`), writes a copy into the
build directory with only `MALLOC_CAP_INTERNAL` in `XFER_DESC_LIST_CAPS` replaced
by `MALLOC_CAP_SPIRAM`, and swaps that copy into the usb component's sources. If
the hash or the fragment does not match, the build fails. The build log prints
`Using ExoAnchor USB descriptor lists in PSRAM`.

Result: over 5 cycles video came back after 1.8 / 1.7 / 1.9 / 2.1 / 5.6 s, and
the failed-allocation callback reported no more USB DMA allocation failures.
The callback and the H.264 handshake deferral from section 4 were then removed,
the "root-port cycle skipped" log was lowered to debug level, the host test
suite passed, and the final verification followed (section 5).

## 4. Tried and not adopted

| Approach | Result | Why |
| --- | --- | --- |
| Enabling `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` as a whole | Video recovers (4 cycles: 1.7 / 2.2 / 15.6 / 1.6 s), but 1080p25 measured alone is only 9.76 fps, the 40 s windows during the cycles 8.86–13.53 fps (source 9.71–18.89 fps); 6 `invalid MJPEG SOI` on serial versus 0 on the comparison build | The option also moves the URB data buffers to PSRAM; moving only the descriptor lists is enough |
| Making an H.264 session wait for the UVC stream to open before claiming codec resources (USB pipes first, codec next) | Video still does not recover; the viewer was closed 77 times after 0 access units; the largest internal DMA block at failure was still 624–1440 B | Deferring the codec did not leave a large enough contiguous block in the internal DMA heap |

## 5. Final measurements

2026-10-28, final build (`build-v24-dev-088-noauth`, app sha256 `4d0f24e5…`),
HTTPS, bench no-auth image.

1080p25 H.264 measured alone for 40 s before the cycles: 24.17 fps, source
24.13 fps, 0 source drops, 0 backpressure drops, average device-side latency
113.7 ms.

Then, with a viewer continuously pulling 1080p25 H.264, the MS2109 was switched
off for 4 s and back on, 6 times:

| Cycle | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Power-on to frames flowing again | 1.9 s | 1.9 s | 1.8 s | 1.7 s | 1.7 s | 1.8 s |

- 0 board reboots, 0 `/api/status` failures; uptime grew continuously to 464 s.
- Serial: 0 `HCD Pipe alloc`, 0 `EP Alloc`, 0 abort / Guru Meditation,
  0 `task_wdt`; 6 capture-card disconnects and 9 H.264 `pipeline ready`; the
  re-plug path never fired.
- The host test suite (Python 3.11) passed.

## 6. Open issues

- The fix is not committed. The project rule is to build with login enabled and
  confirm login works before committing.
- In the final test every cycle enumerated on the first try, so the MS2109
  re-plug path was never exercised. It only ran before the memory fix: it fired,
  but enumeration still failed for lack of memory, and it was accompanied by
  `si_ms2109_power_get_status(210): MS2109 power status busy` errors.
- During the cycles the viewer's first 4 windows of 40 s ran at 20.69–20.96 fps
  and the next 4 at 24.10–24.17 fps; the cause has not been investigated.
- After any reboot, the Agent `TASKS.LOG` replay takes 45–58 s, during which
  CPU 0 triggers `task_wdt` and many HTTPS handshakes fail. A separate session
  has been proposed for it ("Speed up Agent TASKS.LOG boot recovery").
- In the first serial capture, the software reset after one of the aborts hit an
  `Instruction access fault` during startup and was then reset by the LP watchdog
  (`CHIP_LP_WDT_RESET`); the other two aborts booted normally. Not analyzed here.
