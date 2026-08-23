---
name: configure-uart-cli
description: Configure and validate an Ubuntu serial login CLI over ExoAnchor target UART or a USB ACM adapter. Use for UART, serial-console, ttyACM, agetty, serial-getty, baud-rate, login-prompt, reconnect, or missing-console-output setup and diagnosis.
---

# Configure Ubuntu UART CLI

## Guardrails

1. Call `uart_status`, then `uart_read` with `cursor=0`. UART availability is independent from SSH.
2. If `manual_terminal_connected=true`, keep the manual Terminal attached as an output observer. Agent/MCP mutations must temporarily disable its input, identify the automation actor, mirror input/output, and remain stoppable by the browser operator.
3. Never send passwords, private keys, API keys, tokens, or other secrets through UART.
4. Do not claim that a USB `ttyACM*` adapter provides early kernel or bootloader console output. It normally appears only after USB enumeration. Native `ttyS*`/`ttyAMA*` kernel-console setup is a separate, hardware-specific task.
5. This skill supplies procedure only. It does not bypass tool policy, Request Broker approval, a control lease, or host authentication.

## Inspect the Ubuntu endpoint

Prefer a stable device identity, then resolve the kernel TTY before changing systemd:

```sh
ls -l /dev/serial/by-id/
readlink -f /dev/serial/by-id/<adapter-id>
systemctl status dev-ttyACM0.device --no-pager
```

Replace `ttyACM0` below with the resolved basename. Confirm it is the intended adapter before writing configuration.

## Install the serial login service

Create a drop-in that makes 115200 8N1 primary and preserves 9600 as the fallback:

```ini
# /etc/systemd/system/serial-getty@ttyACM0.service.d/override.conf
[Unit]
After=dev-ttyACM0.device
BindsTo=dev-ttyACM0.device

[Service]
Restart=always
RestartSec=1
ExecStart=
ExecStart=-/sbin/agetty --noreset --noclear --keep-baud 115200,9600 %I vt220
```

Apply it:

```sh
sudo install -d -m 0755 /etc/systemd/system/serial-getty@ttyACM0.service.d
sudo systemctl daemon-reload
sudo systemctl reset-failed serial-getty@ttyACM0.service
sudo systemctl enable --now serial-getty@ttyACM0.service
```

When inspecting line settings, use valid `stty` syntax; do not append a literal `speed` operand:

```sh
sudo stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -ixon -ixoff -crtscts
sudo stty -F /dev/ttyACM0 -a
```

## Verify before completion

Run:

```sh
systemctl cat serial-getty@ttyACM0.service
systemctl status serial-getty@ttyACM0.service --no-pager
journalctl -u serial-getty@ttyACM0.service -n 50 --no-pager
```

Then call `uart_read` from the returned `next_cursor`. Require evidence of a login prompt, reconnect the USB serial adapter once, and verify that the service and prompt recover. Use `uart_write` only after an authenticated shell prompt is proven; send one bounded command at a time and continue reading until `pending_bytes=0`.
