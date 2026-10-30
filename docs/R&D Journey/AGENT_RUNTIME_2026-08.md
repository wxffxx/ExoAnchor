# 2026-08-20 to 2026-08-22 on-device Agent runtime bring-up log (0.87.5-dev → 0.87.6-dev)

English mirror of [AGENT_RUNTIME_2026-08_zh.md](AGENT_RUNTIME_2026-08_zh.md). The
Chinese version is authoritative.

- Period: 2026-08-20 to 2026-08-22
- Firmware: a series of `0.87.5-dev` Agent runtime candidates (runtime-v2, r2, r8, r9c, r10, r11, r19, r23),
  ending with `0.87.6-dev IndigoShore` (r28); ESP-IDF 5.5.5
- Hardware: ExoAnchor PrototypeV2.4 (ESP32-P4 rev3.2, 32 MB PSRAM, 16 MB flash), the same board throughout
- The runtime design is in [Agent Runtime V2](../../device/ESP32P4/firmware/v0.86.6-dev/AGENT_RUNTIME_V2.md)
  (Chinese) and the per-change list is in the
  [firmware version history](../../device/ESP32P4/firmware/docs/FIRMWARE_VERSION_HISTORY_zh.md) (Chinese); this
  document records the on-hardware verification methods, data, root causes, and dead ends
- The changes were merged into the main repository on 2026-08-23 as
  `e6d4007d feat: integrate ExoAnchor 0.87.6 agent runtime`

## 1. Summary

| Item | At the start (08-20) | At the end (0.87.6-dev r28) |
| --- | --- | --- |
| Runtime V2 candidate usable on hardware | The first candidate was completely unusable after flashing and hit a panic reboot | Boot, HTTP and MCP work; no panic, watchdog or OOM |
| Agent task stack | Requested a 49152 B internal stack while the largest free internal block was 29696 B; the task could not be created | 24576 B internal stack; high-water 12448 B after a real run |
| Main executor / SSH executor stack frame | 7552 B / 10592 B | 2704 B / 704 B |
| Real cloud run | Could not start | UART0 closed loop: cloud 200, full result reassembled from chunks with a matching CRC |
| Conversation history | — | Written to storage immediately and kept across a software reboot (5208 B / 16 records) |
| H.264 after an Agent run | — | 120/120, 0 encode errors |
| Attribution of MCP video observation | Labelled Agent, generation 0 | `actor=mcp`, `generation=3`; a manual stop by that generation succeeded |
| Runtime V2 as the only executor | No (shadow) | Still no: the model and tools are still executed by the legacy Agent loop |

The runtime logic already passed the host tests; the first thing that stopped it on hardware was internal RAM: the
internal stack the Agent task asked for was larger than the largest contiguous internal block on the live device.
After halving the stack and moving large buffers to the heap, the task was created and cleaned up reliably. From
then on progress was limited mainly by the lack of an Agent API key on the device and the lack of a browser
session, so a UART0 diagnostic entry was added to complete a real closed loop. `0.87.6-dev` is a patch checkpoint
the operator asked for; it does not mean the Runtime V2 cutover is complete.

## 2. Method

**Identity gate**: before every write, the physical model, CH343 serial number, eFuse MAC, silicon revision
(rev3.2), 16 MB flash and board profile were checked against an identity registry that is kept out of the
repository; the serial port was confirmed free and no other session's serial monitor was killed. MAC and silicon
revision were checked again after the write.

**Flashing**: esptool 4.12.0 from the ESP-IDF 5.5 environment, 460800 baud, DIO / 80 MHz / 16 MB, no full-chip
erase, NVS and the TF card preserved. The 0.87.5 candidates wrote four segments (bootloader, partition table, OTA
data, application); r28 wrote only the application at `0x20000`. After every write a separate `verify_flash` run
checked the digests independently.

**Candidate rules**: each candidate has a hardware acceptance matrix, and the version number may change only when
every row passes. A candidate that fails on hardware is quarantined and a superseded candidate is retired; neither
is flashed again, and a fix must go through the identity gate again under a new candidate ID.

**Host side**: the full `tests/host/run.sh`, Toolkit, a fresh ESP-IDF 5.5.5 build, image checksums, an ELF
stack-frame audit, and the application size compared with the smallest 4 MB application partition (`0x400000`).

**On-board observation**: boot logs (panic, Guru, watchdog, stack overflow, OOM), whether anonymous access to
protected APIs returns 401, Agent task stack high-water, the largest contiguous internal heap block, and the power
action counter (to confirm a read-only run caused no physical side effect).

## 3. Problems and root causes

In the order they were found.

### 3.1 The first Runtime V2 candidate was completely unusable on hardware (08-20)

Symptom: after flashing, live operation was completely unusable; the device rebooted again after the recorded boot,
with reset reason panic.

Root cause (source audit): every Runtime V2 write was blocked at the legacy dispatch seam, and read-only runs called
the Verifier with no evidence, so they could never complete.

Handling: the candidate was quarantined. The next one, r2, cut the Task Service submit stack use from about 6592 B
to 400 B and brought the synchronous HTTP submit chain to about 3984 / 12288 B; but a following P0/P1 audit found
gaps in recovery-scope SSH, dry-run, screenshot/video demand and storage isolation, and r2 was retired without
being flashed.

### 3.2 Falling back to the read-only recovery build r8

r8 used a RAM-only recovery composition with features frozen at the legacy read-only scope. It partly passed on
hardware: identity, four-segment write and verify, boot, page assets, stable boot ID, anonymous access boundaries,
and the admission rejection of a SYSTEM dry-run. There was no active browser session after flashing, so the
authenticated Agent and single-tab KVM positive tests could not run.

### 3.3 r9c cutover: the Agent task stack could not get internal RAM (08-21)

r9c enabled the durable Runtime V2 Web binding and disabled the RAM-only recovery composition; the legacy
model/tool loop stayed as a bounded compatibility executor, and writes, HID, power, SSH and UART writes were all
still refused. Host tests, build and image checks passed; on hardware, boot, the 32 MB PSRAM self-test, TF, Ethernet
100 Mbps full duplex, HTTP and anonymous 401 all passed as well.

Symptom: the first real SYSTEM no-screenshot dry-run failed before any model or tool call; the run was persisted in
the failed terminal state and the power action counter stayed at 0.

Root cause: the executor used `xTaskCreateWithCaps` to request a 49152 B internal-only task stack, while the live
device had 55203 B of internal memory free with a largest contiguous block of only 29696 B, so it returned
`ESP_ERR_NO_MEM`.

Handling: r9c was quarantined and is not flashed again.

### 3.4 r10: halving the stack and moving large buffers off it

The cache-safe internal stack capability (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) was kept and the task stack was
not moved to PSRAM. Instead:

- `AGENT_RUN_TASK_STACK` was reduced to 24576 B;
- the 1801 B `latest_message` buffer is allocated from the PSRAM-preferred heap;
- the unused 1801 B caller report buffer was removed;
- the 1801 B operability guard buffer is allocated only when needed;
- the run fails closed if the required message buffer cannot be allocated.

The compiled stack frame of `agent_run_execute_to_json` went from 7552 B to 2704 B.

On hardware: with the same 29696 B largest internal block, the 24576 B task started normally; 20 consecutive
create, fail and secure self-delete cycles all passed, the lowest high-water was 17380 B (threshold 6144 B), the
largest internal block stayed at 29696 B, power actions stayed at 0, and there was no panic, stack canary or
cleanup-task failure. Full model execution was blocked: no Agent API key was configured on the device.

### 3.5 r11: the 10592 B stack frame of the SSH executor

Symptom: an object-file audit after the r10 hardware run found that the compiled SSH execution helper still had a
10592 B stack frame. That write path is closed in the read-only candidate, but the final runtime candidate had to
move the SSH config to the heap first.

Handling: the SSH execution config is allocated from the PSRAM-preferred heap and the credentials are cleared
securely after use.

| Function | r9c | r11 |
| --- | ---: | ---: |
| Agent task | 1072 B | 1072 B |
| Main executor | 7552 B | 2704 B |
| SSH execution helper | 10592 B | 704 B |

On hardware: the task started and ended normally with a 17380 B high-water; the positive run failed closed before
any cloud request, again because there was no API key. Pages returned 200 and anonymous protected APIs returned
401, but there was no authenticated session. The decision was not to reflash for a configuration problem; the
operator would configure the model and key in the authenticated Settings page before continuing.

### 3.6 r19: conversation history written immediately

History writing became a single, bounded writer task: the payload lives in PSRAM and the task uses an external
stack; the user message is fsynced before the assistant reply is written; records are idempotent by record ID; a
half-written record can be recovered; and writes have a timeout bound.

On hardware, a real cloud model (DeepSeek) dry-run returned 200 and the expected `HISTORY_R19_OK`; `HIST.LOG` went
immediately from 0 B / 0 records to 623 B / 2 records; the writer's external-stack high-water was 3276 B and the
Agent internal-stack high-water 12416 B; H.264 after the Agent run passed 120/120; after a software reboot the
623 B / 2 records were kept, and the authenticated Agent page showed the conversation's 2 messages.

### 3.7 r23: a remote Agent closed loop over UART0

Background: after the r23 reboot there was no active browser session (`browser_active=0`), so HTTP could not be
driven with a real browser identity; the MCP identity has no `AGENT_RUN` permission by default, and an MCP token
cannot be used to impersonate one.

Approach: UART0 (the board's local CH343/USB diagnostic port, not exposed to the LAN) became a SYSTEM diagnostic
source granted only `OBSERVE | AGENT_RUN`, with no SSH, HID, power or approval rights. pause, cancel and abort stop
a run with just the exact run ID; resume and steer also require the same diagnostic source, so it cannot borrow a
higher-privilege browser run to escalate.

The HTTP debug surface was implemented in both the firmware and the host tool and passed the host contract tests:
submit distinguishes 202 newly accepted, 200 idempotent duplicate and 409 busy/conflict; a stable `receipt_busy` is
kept separate from a later live `busy`; exact `status?run_id=`, an events cursor and
pause/resume/abort/cancel/steer; the client fails closed on contradictory run aliases, a missing accepted field, a
wrong control and unexpected HTTP status codes; the credential file must be a regular file with mode 0600, and HTTP
needs an explicit `--allow-http`. A Bearer token on port 80 may only be used on a trusted LAN or through a secure
tunnel.

The hardware results are in 5.1.

### 3.8 r28: the boundary between automation and manual control (0.87.6-dev)

Design: power, HID, UART, SSH, video configuration, video hardware and system maintenance are managed through one
resource state, and every Agent, MCP and SYSTEM operation carries an actor, operation ID, run ID and exact
generation. The top navigation shows which resources automation is using; conflicting manual input is temporarily
disabled while read-shared video observation stays available; the operator stops an operation by its current
generation, so a task started later is never stopped by mistake. Automated UART and SSH input is mirrored to the
Terminal page and can be replayed, and manual Terminal use and automated writes are mutually exclusive. KVM and
Agent/MCP share read-only JPEG frames; only an HID input takeover blocks manual input.

Symptom: the first 0.87.5 hardware run found two coupled defects: MCP video observation was labelled Agent, and the
video observation generation was 0.

Fix: video observation distinguishes Agent from MCP by the authenticated principal and updates its epoch on start,
on actor change and on release; after release the old long-lived stream exits, and an old keepalive can no longer
revive a stopped observation.

Version: the operator explicitly asked for the patch checkpoint to be bumped to `0.87.6-dev`, superseding the
earlier rule that every gate must pass before a version change; this bump does not mean the Runtime V2 cutover is
complete.

## 4. Tried and not adopted

| Approach | Result | Reason |
| --- | --- | --- |
| Move the Agent task stack to PSRAM | Not adopted | Keep the cache-safe internal stack and avoid an unsafe external stack; shrink the stack and move large buffers to the heap instead |
| Reflash because the API key is missing | Not adopted | It is a configuration problem and belongs in the authenticated Settings page |
| Use an MCP token for the authenticated HTTP Agent test | Not adopted | The MCP identity has no `AGENT_RUN` by default and cannot impersonate a browser identity |
| The system esptool 4.7.0 | Its failure was not counted | esptool 4.12.0 from the ESP-IDF 5.5 environment is used throughout |

## 5. Final measurements

### 5.1 r23 UART0 closed loop (2026-08-22)

Host side: firmware host suite passed, HTTP Agent CLI mock/contract 11/11, Toolkit 77/77.

| Item | Result |
| --- | --- |
| Positive run | Cloud 200, reply exactly `UART_REMOTE_R23_OK` |
| Status and events | Run ID matches; the events cursor and the `done` terminal state are readable |
| Full result | 2206 B, base64 chunks reassembled, CRC32 `d908ae85` |
| Conversation history | `history_saved=true`, `history_pending=false`; 3930 B / 12 records → 4566 B / 14 records |
| Resources | Agent internal-stack high-water 12448 B; history writer external-stack high-water 3276 B |
| Concurrent submit | A second submit returns `accepted=0,busy=1`, creates no new run, and the active ID points at the current run |
| Exact control | steer, pause, resume and cancel on the second run were all accepted; terminal state `aborted` |
| Wrong run ID | Returns `run_id_mismatch` without leaking another run's result |
| H.264 after the Agent run | 120/120, 0 encode errors, SPS/PPS/IDR all present |
| Network read | `network show` 100/100 in a row, no cache assert or reset |
| Network change from an external stack | Returns `ESP_ERR_INVALID_STATE`; the active network is unchanged |
| Software reboot | History kept at 5208 B / 16 records |
| Anonymous HTTP | Agent status/events/history/sessions/requests all return 401 |

### 5.2 r28 (0.87.6-dev IndigoShore, 2026-08-22)

- Application 3041216 B (`0x2e67c0`), 27% of the smallest 4 MB application partition free; SHA-256
  `e7bf95b20f231ed8b8abc0b298ba99c72f1798c0f0a41881469c9d95fb4e1c06`.
- Application-only write; the write hash and an independent `verify_flash` matched, MAC and silicon revision
  matched after the write, and the firmware reports `exoanchor-prototype-v2.4 / 0.87.6-dev`; the UI shell SHA-256
  on the device matches the local source.
- Ethernet 100 Mbps full duplex, TF mounted, HTTP online, HID ready, UART1 initialized at 115200 8N1; boot logs show
  no panic, Guru, watchdog, stack overflow, abort or OOM.
- Authenticated MCP capability and status queries and a 1280×720 JPEG snapshot passed.
- During the MCP snapshot, an authenticated browser read `actor=mcp`, `resource=video-observe`, `generation=3`,
  `blocks_manual=false`, `manual_takeover_available=true`; a browser stop by that generation returned
  `accepted=true`, and the active operation count was 0 right after.

### 5.3 Application size

| Candidate | Application size |
| --- | ---: |
| 0.87.5 Settings baseline (08-20) | 2880464 B |
| r8 | 2934016 B |
| r9c | 2955104 B (`0x2d1760`) |
| r10 | 2955360 B |
| r11 | 2955408 B |
| r23 | 3008464 B (about 28% free) |
| r28 | 3041216 B (27% free) |

## 6. Open issues

- Runtime V2 is not yet the only executor: the model and tools are still executed by the legacy Agent loop, and V2
  only provides the shadow binding, event log and status projection. Before writes can be opened it still needs a
  target fingerprint for each tool type, an atomic Execution Gateway at the owning-manager boundary, typed readback
  that can issue Verifier evidence, and fault injection for power loss, lost responses, lease loss, manual KVM
  takeover and so on.
- The full Agent loop over authenticated HTTP (submit → events/control → result → history) has not run on the
  device yet; it needs a real browser session.
- Still open from r23: `include_screenshot=true` being refused with the KVM owner unchanged; the internal SSH TOFU
  broker on hardware; the 120 s internal rollback after a network apply; ACD conflict/timeout rollback for a static
  IP; persistence of feature-off, reboot, and early UART refusal.
- Still open from r28: the complete matrix of simultaneous browser KVM and automation input plus neutral, Terminal
  automated-input replay and fault injection, H.264 recovery, and a long soak.
- Write tools, HID/lease takeover, and the M0, T1 and T2 release gates are not done; `0.87.6-dev` is not an RC or
  Stable.
- Since 2026-10-23, writes by the built-in Agent are governed by the automation level set by the operator (L0–L4,
  default L1 read-only) plus the action category decided by the firmware, instead of a blanket refusal; Runtime V2
  is still in shadow and the cutover hard gates are unchanged.
