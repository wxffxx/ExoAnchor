# ExoAnchor MCP Controller

This package exposes an ExoAnchor ESP32-P4 KVM device as an MCP stdio server for Codex-like clients.

It is intentionally separate from the built-in firmware Agent:

- It does not call `/api/agent/*`.
- It uses MCP tool names prefixed with `exoanchor_`.
- HID control uses a dedicated control owner, `mcp`, on firmware that supports it.
- Device Settings includes an MCP Controller switch that can block this external controller.
- Mutating tools are disabled until `EXOANCHOR_ALLOW_WRITE=1` is set.

The staged protocol, safety, observation, deterministic-control, and Codex
verification plan is documented in [`docs/ROADMAP_zh.md`](docs/ROADMAP_zh.md).
Module boundaries and state ownership are documented in
[`docs/ARCHITECTURE_zh.md`](docs/ARCHITECTURE_zh.md).
Generic zero-shot BIOS navigation and operating-system installation are not
current MCP acceptance targets.

## Tools

| Tool | Class | Notes |
| --- | --- | --- |
| `exoanchor_capabilities` | Read-only | Firmware and bridge capability observation. |
| `exoanchor_status` | Read-only | Provenance-wrapped device/video/HID/power/SSH/lease status. |
| `exoanchor_snapshot` | Read-only | JPEG observation with time, dimensions, SHA-256 and frame ID. |
| `exoanchor_logs` | Read-only | Stable pagination over one cached log observation. |
| `exoanchor_observation_get` | Read-only | Replay one recent observation from this server lifetime. |
| `exoanchor_wait_for_status` | Read-only | Bounded wait over a fixed named condition. |
| `exoanchor_wait_for_frame_change` | Read-only | Bounded wait for an exact JPEG hash change. |
| `exoanchor_open_kvm` | Read-only | Human KVM page URL. |
| `exoanchor_click_pixel` | Destructive | Frame-bound pixel-to-absolute-HID click. |
| `exoanchor_type_text` | Destructive | Frame-bound printable US-layout text. |
| `exoanchor_execute_and_observe` | Destructive | Frame-bound HID batch with cleanup and verification snapshot. |
| `exoanchor_ssh_job_start/status/cancel/result` | Mixed | Structured bounded job lifecycle and paginated result. |
| `exoanchor_ssh_exec` | Destructive | Compatibility escape hatch; arbitrary SSH is disabled by default. |
| `exoanchor_control_lease` | Destructive | Acquire or release the MCP HID lease. |
| `exoanchor_hid_actions` | Destructive | Strictly validated, supervised HID action batch. |
| `exoanchor_power_action` | Destructive | Power/reset/force-off/locator actions. |
| `exoanchor_video_lease` | Destructive | Preview or KVM video ownership. |

## Safety Contract

- The server negotiates MCP protocol `2025-06-18` and publishes server-wide
  instructions during initialization.
- Every tool declares MCP read-only/destructive/idempotent/open-world hints.
  These hints help clients present risk; they do not replace device-side policy.
- Mutating tools remain disabled unless `EXOANCHOR_ALLOW_WRITE=1` is set.
- HID and control-lease operations default to `supervised`, not `autonomous`.
- HID action objects are validated by action type; undeclared fields are rejected.
- SSH passwords and private keys are not accepted as tool arguments. Configure
  credentials in the device secret store and use stored authentication.
- Structured SSH jobs use idempotency keys, sanitized local audit journals and
  bounded firmware requests. Cancellation is cooperative; a bridge restart
  marks active jobs `interrupted` instead of guessing the remote result.
- Firmware currently reports `host_key_check=false`; SSH is therefore rejected
  unless host verification is implemented or a supervised exception is
  explicitly enabled.
- Pixel and text actions require the exact current JPEG frame ID. Coordinates
  from an older frame are rejected before the control lease is requested.
- Generic zero-shot BIOS navigation is outside this server contract. An unknown
  BIOS/UEFI page requires observation and human supervision.

## Configuration

Environment variables:

| Name | Default | Meaning |
| --- | --- | --- |
| `EXOANCHOR_BASE_URL` | required | Device base URL, for example `http://<device-ip>`. |
| `EXOANCHOR_USERNAME` | unset | Local device username; required for password login. |
| `EXOANCHOR_PASSWORD` | unset | Local device password. Prefer this over embedding a token. |
| `EXOANCHOR_PASSWORD_FILE` | unset | File containing the local device password. |
| `EXOANCHOR_TOKEN` | unset | Optional bearer token. |
| `EXOANCHOR_TOKEN_FILE` | unset | File containing the bearer token. |
| `EXOANCHOR_TIMEOUT` | `75` | HTTP timeout in seconds; bounded synchronous SSH remains at most 60 seconds. |
| `EXOANCHOR_ALLOW_WRITE` | `0` | Set to `1` to enable SSH/HID/power/video-changing tools. |
| `EXOANCHOR_CONTROL_OWNER` | `mcp` | Control lease owner. Use `mcp` on firmware with MCP owner support. |
| `EXOANCHOR_DEVICE_ID` | URL hostname | Stable configured-device identity checked by action tools. |
| `EXOANCHOR_STATE_DIR` | `~/.local/state/exoanchor-mcp` | Private SSH job audit journal. |
| `EXOANCHOR_PERSIST_JOB_OUTPUT` | `0` | Persist potentially sensitive SSH output in the private journal. |
| `EXOANCHOR_ALLOW_UNVERIFIED_SSH_HOST` | `0` | Supervised exception for firmware without host-key checking. |
| `EXOANCHOR_ALLOW_ARBITRARY_SSH` | `0` | Enable the arbitrary-command escape hatch. Structured recipes remain preferred. |

Device-side access switch:

1. Open `Settings`.
2. Go to `Agent、Provider 与 Skills`.
3. Use `MCP Controller` to enable or disable external MCP access.

## Run Locally

From this directory:

```bash
python3 -m exoanchor_mcp.server --list-tools
EXOANCHOR_BASE_URL=http://<device-ip> EXOANCHOR_USERNAME='<user>' EXOANCHOR_PASSWORD_FILE=/path/to/password-file python3 -m exoanchor_mcp.server --probe
```

Install as an editable package:

```bash
python3 -m pip install -e .
```

Then run:

```bash
exoanchor-mcp
```

## MCP Client Example

Generic MCP JSON config:

```json
{
  "mcpServers": {
    "exoanchor": {
      "command": "python3",
      "args": [
        "-m",
        "exoanchor_mcp.server"
      ],
      "cwd": "/absolute/path/to/ExoAnchor/integrations/exoanchor-mcp",
      "env": {
        "EXOANCHOR_BASE_URL": "http://<device-ip>",
        "EXOANCHOR_USERNAME": "<device-user>",
        "EXOANCHOR_PASSWORD_FILE": "/path/to/private/device-password"
      }
    }
  }
}
```

Codex-style TOML:

```toml
[mcp_servers.exoanchor]
command = "python3"
args = ["-m", "exoanchor_mcp.server"]
cwd = "/absolute/path/to/ExoAnchor/integrations/exoanchor-mcp"
required = true
tool_timeout_sec = 90
enabled_tools = [
  "exoanchor_capabilities",
  "exoanchor_status",
  "exoanchor_snapshot",
  "exoanchor_logs",
  "exoanchor_observation_get",
  "exoanchor_wait_for_status",
  "exoanchor_wait_for_frame_change",
  "exoanchor_open_kvm",
]

[mcp_servers.exoanchor.env]
EXOANCHOR_BASE_URL = "http://<device-ip>"
EXOANCHOR_USERNAME = "<device-user>"
EXOANCHOR_PASSWORD_FILE = "/path/to/private/device-password"
```

Start Codex verification with this read-only allowlist. After read-only tests
pass, add only the required mutating tools to `enabled_tools`, set
`default_tools_approval_mode = "writes"` in the server table, and add
`EXOANCHOR_ALLOW_WRITE = "1"` to the environment. Do not enable every mutating
tool for convenience.

Ready-to-copy project-scoped examples:

- [`docs/codex/readonly.toml.example`](docs/codex/readonly.toml.example)
- [`docs/codex/supervised.toml.example`](docs/codex/supervised.toml.example)

## Verification

Run protocol and safety-contract tests without a device:

```bash
python3 -m unittest discover -s tests -v
```

The suite includes a replayable local HTTP device and covers read-observe,
frame-bound HID, cleanup, structured SSH jobs and idempotency without touching
real hardware.

Run the live Stage 4 read-only acceptance sequence:

```bash
EXOANCHOR_BASE_URL=http://<device-ip> \
EXOANCHOR_USERNAME='<user>' \
EXOANCHOR_PASSWORD_FILE=/path/to/password.secret \
python3 scripts/stage4_acceptance.py --output /path/to/report.json
```

The script refuses to run if `EXOANCHOR_ALLOW_WRITE` is enabled. It does not
acquire video/HID leases, send SSH commands, change power, or flash firmware.

Stage 4 acceptance reports are generated under `docs/acceptance/` and retained
locally. They are intentionally ignored because live captures can contain
private addresses, device fingerprints, observation IDs, and runtime state.

Then perform real-device verification in this order:

1. `exoanchor_status`
2. `exoanchor_logs`
3. `exoanchor_snapshot`
4. supervised HID on a fixed, non-destructive test screen
5. lease-conflict and disconnect recovery

Power, reset, force-off and arbitrary SSH are not part of the initial read-only
Codex smoke test.

Pure-KVM firmware images do not expose the dev MCP API. The bridge reports a
firmware compatibility error when `/api/settings/mcp` or `/api/capabilities` is
missing; it does not classify that case as a generic network failure.

## HID Action Examples

Send `Ctrl+Alt+Del`:

```json
{
  "actions": [
    {"type": "keydown", "code": "ControlLeft"},
    {"type": "keydown", "code": "AltLeft"},
    {"type": "keydown", "code": "Delete"},
    {"type": "wait", "ms": 50},
    {"type": "keyup", "code": "Delete"},
    {"type": "keyup", "code": "AltLeft"},
    {"type": "keyup", "code": "ControlLeft"}
  ],
  "auto_lease": true,
  "release_after": true
}
```

Click absolute HID coordinate:

```json
{
  "actions": [
    {"type": "absclick", "x": 32767, "y": 32767, "button": 0}
  ],
  "auto_lease": true
}
```

## Compatibility

Firmware before MCP owner support only accepted `owner=agent` for `/api/control/lease` and `/api/hid/actions`.
Keep `EXOANCHOR_CONTROL_OWNER=mcp` for non-conflicting behavior. If you deliberately target older firmware, set `EXOANCHOR_CONTROL_OWNER=agent`, but that compatibility mode shares the same owner identity as the built-in Agent.
