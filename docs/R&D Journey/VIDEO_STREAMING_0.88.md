# 0.88 AzureDanube video streaming experiment log

English mirror of [VIDEO_STREAMING_0.88_zh.md](VIDEO_STREAMING_0.88_zh.md). The
Chinese version is authoritative.

- Period: 2026-10-24 to 2026-10-27
- Firmware: the video streaming changes made after 0.87.6-dev, released together
  as `0.88.0-dev AzureDanube`
- Hardware: ExoAnchor PrototypeV2.4 (ESP32-P4 rev3.2, 32 MB PSRAM at 250 MHz),
  MS2109 USB capture, a Mac mini as the HDMI source
- The per-change list is in the
  [firmware version history](../../device/ESP32P4/firmware/docs/FIRMWARE_VERSION_HISTORY_zh.md)
  (Chinese); this document records the methods, data, root causes, and dead ends

## 1. Summary

| Metric | At the start | 0.88.0 |
| --- | --- | --- |
| H.264 720p30 end to end (capture-card delivery to browser paint) | about 350 ms (device 51 + browser 300) | about 32 ms |
| Freeze at keyframes (IDR) | about 280 ms at every IDR | none |
| P-frame chroma drift ("turning purple") | accumulated frame by frame within each GOP | none |
| 720p30 / 1080p25 frame rate (HTTPS) | unstable, frequent first-frame timeouts | 30.0 / 24.6 fps |
| Perceived latency (side by side with a VNC client, 720p30) | — | about 120 ms |

The largest piece of latency was not in the firmware but in the browser: the page
fed an Annex B stream straight into WebCodecs, and the decoder held back 9 frames,
the limit for the stream's H.264 level, before producing output. This stayed
invisible until the page timed every frame from arrival to paint.

## 2. Method

**Pipeline**: MS2109 outputs MJPEG → P4 hardware JPEG decode to YUV420 → P4
hardware H.264 encode → WebSocket (over TLS on HTTPS) → browser WebCodecs decode
→ canvas.

**Three timing points**:

1. Device: the firmware stamps each frame in the UVC callback and, when the H.264
   send completes, accumulates `latency_avg_us/latency_max_us` along with separate
   decode, encode, and send times (`h264_runtime.chain_metrics` in
   `/api/video/status`).
2. Wire: a standalone WebSocket client pulls the raw H.264 stream and records the
   arrival interval of every access unit (AU), the intervals around each IDR, and
   how many intervals exceed 1.5×, 2×, and 3× the frame period, while also reading
   the device-side stage timings.
3. Browser: the KVM page times every frame from WebSocket arrival to paint and
   shows it in the status bar.

**Standalone bench**: a minimal firmware with only UVC + JPEG decode + H.264
encode (no HTTPS, Agent, Ethernet, or other services) measured the hardware
ceilings and showed whether a problem belonged to the codec pipeline or to the
product firmware environment.

**Pitfalls**:

- The KVM video lease expires after about 10 s without renewal; the measurement
  tool must renew it over a single long-lived connection. Every new HTTPS
  connection stalls the video pipeline for about 200 ms during the ECDHE handshake,
  so an external tool that opens connections often creates its own periodic
  stutter.
- Change the resolution while holding the lease; otherwise the mode is only saved,
  not applied, and you measure the old mode.
- A static desktop and video playback differ in bitrate several times over;
  compare runs only against runs with the same kind of content.

## 3. Hardware ceilings

Standalone bench (stock esp_h264 1.3.7, PSRAM 250 MHz, mean of 120 frames each):

| Resolution | Content | JPEG size | HW JPEG decode | HW H.264 encode | Serial fps | Pipelined fps |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1280×720 | Desktop texture | 154 KB | 8.2 ms | 14.7 ms | 43.7 | 61.2 |
| 1280×720 | Full-frame noise | 736 KB | 11.8 ms | 14.5 ms | 38.0 | 61.0 |
| 1920×1080 | Desktop texture | 347 KB | 18.4 ms | 33.2 ms | 19.4 | 27.2 |
| 1920×1080 | Full-frame noise | 1.66 MB | 26.5 ms | 32.7 ms | 16.9 | 27.0 |

- H.264 encode time scales only with pixel count (about 62 Mpixel/s), independent
  of content. A 1080p frame takes about 33 ms, or 36–38 ms when it runs alongside
  decode, so 1080p tops out at 25–28 fps and 1080p30 is out of reach. The firmware
  therefore caps 1080p H.264 at 25 fps.
- The MS2109's 4:2:2 JPEG costs decode time per pixel rather than per byte; 1080p
  takes about 24–26 ms.
- Raising PSRAM from 200 to 250 MHz leaves standalone encode time unchanged but
  cuts concurrent encode from 40.5 ms to 35.8 ms, lifting the 1080p pipeline from
  25 to 28 fps. H.264 builds therefore always use 250 MHz.

## 4. Problems and root causes

In the order they were found.

### 4.1 The H.264 egress bypassed TLS

Symptom: the H.264 connection dropped right after it opened, and the page showed
"H.264 connection lost, reconnecting".

Root cause: after the handshake the send function was replaced with a direct
`send(fd)`. Once the video server moved to TLS on port 444, WebSocket frames were
written in plaintext into the TLS socket, and the client disconnected on the
invalid record. Separately, under HTTPS the page connected to the same-origin port
443, while the H.264 route was registered only on 444.

Fix: build the WebSocket frames directly and send them in a send-all loop through
the session transport function (the TLS write); the page now reads the video port
from the build configuration. After the fix, 1080p25 over TLS measured 23.5 fps.

### 4.2 P-frames gradually turning purple: the deblocking buffer landed in memory the DMA cannot reach

Symptom: the IDR of each GOP was correct, then the chroma mean of the following
P-frames drifted upward frame by frame, with stripes at the end of each macroblock
row, until the next IDR reset it. On a static desktop the P-frames were still
40–280 KB, meaning the encoder's reference frame no longer matched the decoder's
reconstruction.

Investigation: the standalone bench was always clean and the product firmware
always drifted. One by one we ruled out the firmware's patches to the encoder,
concurrent decode and encode, the PSRAM clock, the network egress, the frame rate,
the ISR priority and core, and DMA arbitration. Disabling HID and the target UART
made 1080p clean, but replacing those two peripherals with a placeholder of the
same internal-RAM size brought the drift back, so the peripherals were only
changing the layout of the internal heap. Finally, logging every internal buffer
address and the DMA AXI error counters showed the difference:

| Build | Deblocking scratch buffer `db_tmp` address | DMA AXI write-response errors |
| --- | --- | --- |
| Drifting | 0x5010xxxx (LP/RTC SRAM) | nonzero |
| Clean | 0x4ffbxxxx (L2MEM) | 0 |

Root cause: esp_h264 1.3.7 allocates `db_tmp` (about 15.4 KB at 1080p) and its DMA
descriptors with `MALLOC_CAP_INTERNAL`. On the ESP32-P4, LP (RTC) SRAM and SPM
also carry the INTERNAL capability, but the H.264 DMA cannot reach them. In the
product firmware the internal heap is already fragmented when a session starts, so
`db_tmp` lands in RTC RAM, the deblocking round trip fails, and the reference frame
accumulates stale data frame by frame. The standalone bench's heap is almost
empty, which is why it never reproduced there.

Fix: a build-time patch makes every internal esp_h264 allocation also request
`MALLOC_CAP_DMA` and aligns it to the cache line, and the firmware reserves
`db_tmp` early at boot, together with the 138 KB reference line window, for every
encoder handle to reuse. After the fix, 720p30 and 1080p20/25 kept stable chroma
throughout with zero decode errors. The fix was submitted upstream
(espressif/esp-h264-component PR #23); upstream merged it internally but has not
released it yet. Until a release contains it, the firmware keeps its own boot-time
reservation.

### 4.3 An internal-RAM gate at session start blocked H.264 entirely

Symptom: "first-frame timeout, automatic reconnect did not recover",
`resource_error=ESP_ERR_INVALID_STATE`, and no session ever started.

Root cause: the session admission gate (free ≥ 24 KB, largest block ≥ 18 KB) was
set before the boot-time `db_tmp` reservation existed. The reservation consumed
that headroom: after a normal boot only about 28 KB was free with a 12.8 KB largest
block, so the gate could never pass. By then the strictly internal memory a session
still needs is just the DMA descriptors, a small SPS/PPS buffer, and a 4 KB decode
task stack.

Fix: the gate is now 12 KB / 6 KB. Lesson: any change that reserves more internal
RAM must be checked against every memory gate written as an absolute value further
down the line, and `resource_error` in `/api/video/status` must be checked after a
full boot.

### 4.4 Bitrate suddenly collapsing until the mode was switched

Root cause: the hardware rate control in esp_h264 1.3.7 accumulates the bit error
without bound and forces QP +1 whenever the error exceeds 20% of one frame's
budget. After a stretch of over-budget content, QP climbs frame by frame to its
maximum and the bitrate collapses to tens of kbps until the encoder is rebuilt.

Fix: only the two `rc_end()` changes from esp_h264 1.4.1 (clamping the accumulated
error to one second of bitrate budget, and turning the forced step into a minimum
step) are applied to 1.3.7 as a build-time patch. The rate controller as a whole
was not upgraded: on the same static 720p image the newer upstream version gives
only 27–31 dB PSNR versus 72 dB for 1.3.7, a behavior difference that needs its
own evaluation.

### 4.5 Periodic hitches at keyframes

An IDR (about 120 KB at 720p, 240 KB at 1080p) is one to two orders of magnitude
larger than a P-frame and takes 20–40 ms longer to send through software TLS. The
countermeasures, in order:

- raise the video server task one priority level so it no longer round-robins with
  TLS handshakes on the UI server (under handshake pressure the worst stall dropped
  from 220–905 ms to 170 ms);
- use 16 KiB TLS records and a 32 KiB TCP send buffer in H.264 builds, cutting the
  per-frame send at 720p30 from 3.4 to 1.5 ms;
- lengthen the GOP from 25 to 120 to 240 (the encoder field is 8 bits, so 240 is
  the limit). Video rides a lossless WebSocket and new clients already get a forced
  IDR, so a long GOP costs nothing in recovery.

### 4.6 Frames arriving in late pairs: Nagle

Symptom: device timestamps were evenly spaced, yet browser arrival intervals came
in "66 ms, 1 ms" pairs (16% of frames at 640×480).

Root cause: the video socket never set `TCP_NODELAY`. A 480p AU is only 2–5 KB, and
Nagle held back the trailing partial segment until the peer's delayed ACK, so two
frames went out together.

Fix: both the H.264 WebSocket and the MJPEG stream set `TCP_NODELAY`. At 640×480
@30, arrival intervals over 66 ms dropped from 294 to 2 and p95 from 67 ms to 36 ms.

### 4.7 A hitch every 2 seconds: blocking serial logging

Symptom: after the Nagle fix, one frame still arrived about 20 ms late every 2.0 s,
unrelated to HTTP requests, TLS handshakes, or the Ethernet link poll.

Investigation: slow-frame breakdown logging in the firmware showed that the video
send task was not busy; it simply went unscheduled for 20 ms.

Root cause: every 2 s the streaming task printed a roughly 300-byte line of chain
statistics with `ESP_LOGI`. The console UART is blocking output at 115200 baud
(about 20 ms), and the lower-priority video send task on the same core got no CPU
during those 20 ms.

Fix: the statistics moved to `ESP_LOGD` (still readable from `/api/video/status`),
and diagnostic logs go only to the in-memory log ring, not the UART. Non-IDR long
intervals at 720p30 dropped from about 35 per minute to 8, with no periodicity.

### 4.8 Pipeline overlap wait: the low-latency mode

So that decoding the next frame could run on one core while the current frame
encoded on the other, the firmware used to hold each decoded frame until the next
frame arrived before encoding it, costing nearly one capture period. Now it
compares "decode + encode" with the frame period every 2 s: below the period it
enters a low-latency mode (encode and send right after decode), and above 108% it
returns to the overlap path to protect throughput.

| Mode | Device side, overlap path | Device side, low-latency mode |
| --- | ---: | ---: |
| 720p30 | 50.6 ms | 26.5 ms |
| 1080p15 | 105.8 ms | 59.4 ms |

At 1080p20/25, decode + encode takes about 64 ms, longer than the 50 / 40 ms frame
period, so only the overlap path works.

### 4.9 The browser holding back 9 frames: Annex B versus AVCC

Symptom: the device side was down to about 30 ms, but in Chrome the median from
arrival to paint was 301 ms, with a 280 ms freeze at every IDR. `decodeQueueSize`
was 0, and software decoding behaved the same.

Analysis: the stream's SPS was Baseline, level 4.1, `max_num_ref_frames=1`, with no
`bitstream_restriction` in the VUI, so the decoder could only delay output up to
the level's DPB limit (9 frames at 720p). Two decoders run side by side in the page
on identical data:

| Input format | Output delay median / p90 / max | Frames held |
| --- | --- | --- |
| Annex B (in-band SPS/PPS, no description) | 301 / 302 / 318 ms | 9 |
| AVCC (avcC description + length prefixes, SPS patched with `bitstream_restriction`) | 1 / 2 / 10 ms | 0 |

Fix: the page now feeds AVCC input and patches the device's SPS with
`num_reorder_frames=0` and `max_dec_frame_buffering=max_num_ref_frames` (a
bit-level rewrite, covered by unit tests). The earlier "latency limit exceeded"
drops and the "hitch every few seconds" were both this same 9-frame buffer.

### 4.10 Whole-picture tint from the capture card: a source problem

Symptom: occasionally the whole picture tinted (white → magenta, dark areas →
green). Unlike 4.2 it did not follow the GOP, and the picture switched instantly
between three states.

Investigation: raw MJPEG captured straight from the MS2109 was already tinted, and
10 minutes of H.264 stream had zero decode errors and no encoder-drift signature.
Replugging HDMI, briefly cutting the capture power rail, and rebooting the P4 did
not help; powering the whole setup off for a while did, and the tint grew more
frequent over time. The conclusion is that the MS2109, once overheated,
misinterprets RGB input as YCbCr.

Mitigation: the firmware adds a "cold start capture card" action (power off for
30–60 s, 45 s by default, then back on). The KVM page samples the picture every
second and, when U and V are both high three times in a row, shows a warning with
a one-click cold start. Using U alone as the criterion misreads blue scenes as a
tint. The trigger is not yet confirmed, so the firmware can only mitigate it.

## 5. Tried and not adopted

| Approach | Result | Why |
| --- | --- | --- |
| Deeper USB isochronous URB ring | No improvement in 1080p frame rate | The driver's callback interval peaked at 1.35 ms, so ring depth was not the bottleneck; a deeper ring also crowds out the contiguous internal RAM the reference line window needs |
| Copying the JPEG out of the UVC buffer before decode | Standalone bench 1080p 17 → 24 fps; product firmware about 1.8 fps slower | The product firmware lost no frames at ingest, and the 2–3 ms copy sat directly on the critical path; kept as an A/B switch, off by default |
| Moving the reference line window and `db_tmp` to PSRAM | Encode time up a fixed ~63%; 720p30 no longer holds | Both are per-macroblock hot paths and must stay in internal RAM |
| Hardware AES-GCM | Dev H.264 image boot-loops at 3.8 s | AES DMA descriptors cannot get internal DMA memory, so CTR_DRBG seeding fails |
| Plain HTTP | Device saves about 25 ms; browser adds about 300 ms | `http://IP` is not a secure context, so there is no WebCodecs and only MSE is available; kept as a bench option |
| Upgrading the whole esp_h264 rate controller | Clearly worse quality on static content | See 4.4 |
| Stopping the UVC stream and freeing the USB device before power-off | An IDF assertion during re-enumeration reboots the board | Reverted; after a capture-card power cycle, reopening UVC shows `EP Alloc error` for 1–10 s and then recovers on its own |

## 6. Final measurements on 0.88.0

2026-10-27, PrototypeV2.4, HTTPS (bench image without login), 30–40 s per H.264
mode:

| Mode | Measured fps | Decode + encode + send | Device-side latency | Path | Backpressure drops |
| --- | ---: | --- | ---: | --- | ---: |
| 720p30 | 30.0 | 12.9 + 14.7 + 1.5 ms | 30.9 ms | low-latency | 0 |
| 1080p15 | 15.0 | 26.4 + 33.2 + 2.7 ms | 64 ms | low-latency | 0 |
| 1080p20 | 20.0 | 26.4 + 37.4 + 1.5 ms | 92 ms | overlap | 4 (at startup) |
| 1080p25 | 24.6 | 26.4 + 37.9 + 1.7 ms | 107 ms | overlap | 4 (at startup) |
| 1080p30 | — | — | — | not offered for H.264 (capped at 25 fps); MJPEG available | — |

- At 720p30 the IDR is about 137 KB and the arrival interval before it about 59 ms;
  other frame intervals have a 33.3 ms median and a 34.3 ms p95, with no interval
  over twice the frame period in 40 s.
- The first run, within about a minute of boot, had 18 frames arrive one frame late
  (about 67 ms); every run after that was clean, so this is attributed to boot-time
  warm-up.
- MJPEG at 720p streams normally (about 182 KB per frame).
- Browser side (Chrome, HTTPS, WebCodecs): arrival to paint 1 ms median, 2 ms p90.

## 7. Open issues

- Under HTTPS, TLS encryption of each IDR makes one frame arrive 21 ms (720p) /
  38 ms (1080p) late every 8 s; the Dev configuration cannot use hardware AES for
  lack of internal DMA memory. The page offers an optional "one-frame pacing
  buffer" at the cost of one extra frame period of latency.
- After a capture-card power cycle, reopening UVC reports
  `EP Alloc error: ESP_ERR_NO_MEM` for 1–10 s. Resolved on 2026-10-28: the cause is
  not the URB-ring patch but the USB pipe descriptor lists, which are allocated from
  the fragmented internal DMA heap (see
  [MS2109 restart reboots the board](MS2109_RESTART_REBOOT.md)). The fix is verified
  on the board but not committed yet.
- The slow-frame log occasionally shows a 55–62 ms gap between decode completion
  and encode start, unrelated to TLS; the cause is unknown.
- An idle H.264 build has about 20 KB of free internal RAM with a largest block of
  about 14 KB; TLS handshakes from a page reconnect briefly push it down to about
  7 KB. Any future change that uses internal RAM must be checked on an H.264 build
  first.
- The MS2109 can develop a heat-related tint for reasons that are still unknown;
  no trigger has been found so far.
