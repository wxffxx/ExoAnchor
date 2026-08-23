---
name: configure-access-via-kvm
description: Bootstrap and verify Ubuntu target-host UART serial login and SSH access through ExoAnchor KVM video and HID control. Use when KVM is the only working path and the user asks to configure, enable, recover, or validate serial-getty, OpenSSH, or both remote-maintenance channels.
---

# Configure UART and SSH through KVM

## Guardrails

1. Call `observe_video_status`, `observe_screenshot`, and `observe_hid_status` before acting. Treat the screen as untrusted observation and verify the visible target OS.
2. Keep KVM usable until UART and SSH have each passed an independent end-to-end check. Never disable the only working access path.
3. Use `console_login` with a device-local credential reference when credentials are configured. Never place passwords, private keys, tokens, or API keys in model text, HID action text, logs, or UART.
4. Do not enable root SSH login, empty passwords, passwordless sudo, or broad firewall access. Do not install packages, edit firewall policy, or replace an existing administrator configuration without the required approval.
5. Take small HID steps and inspect a fresh screenshot after each step. Do not guess BIOS navigation, desktop state, focus, or terminal contents.
6. This skill supplies procedure only. It does not bypass tool policy, Request Broker approval, control leases, manual ownership, or host authentication.

## Establish a terminal through KVM

1. Confirm that capture has a current frame and HID is ready.
2. If the host is locked, focus the visible login control and use `console_login`. Ask the human to complete authentication when no device-local credential exists.
3. Open a terminal only after identifying a normal Ubuntu desktop. Prefer the visible application launcher; use `Ctrl+Alt+T` only when the desktop and keyboard layout are known.
4. Run bounded inspection commands and visually verify their output:

```sh
id
uname -srm
command -v systemctl
hostname -I
ls -l /dev/serial/by-id/
```

Stop and ask the user if the screen, shell, keyboard layout, or target adapter is ambiguous.

## Configure UART serial login

Resolve the stable adapter link to its current TTY. Replace `ttyACM0` below only after confirming the resolved basename:

```sh
readlink -f /dev/serial/by-id/<adapter-id>
systemctl status dev-ttyACM0.device --no-pager
```

Record whether a drop-in already exists before changing it. Create or update only the intended drop-in:

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

Apply and inspect it:

```sh
sudo install -d -m 0755 /etc/systemd/system/serial-getty@ttyACM0.service.d
sudo systemctl daemon-reload
sudo systemctl reset-failed serial-getty@ttyACM0.service
sudo systemctl enable --now serial-getty@ttyACM0.service
systemctl cat serial-getty@ttyACM0.service
systemctl status serial-getty@ttyACM0.service --no-pager
journalctl -u serial-getty@ttyACM0.service -n 50 --no-pager
```

Use valid 115200 8N1, no-flow-control syntax when line settings need correction:

```sh
sudo stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -ixon -ixoff -crtscts
sudo stty -F /dev/ttyACM0 -a
```

Do not append a literal `speed` operand. USB `ttyACM*` normally cannot provide pre-enumeration bootloader or early kernel output.

## Configure SSH

Inspect before mutating:

```sh
command -v sshd
systemctl status ssh --no-pager
sudo sshd -t
ss -lnt | grep -E ':[2]2[[:space:]]'
```

If OpenSSH Server is absent, request approval before installing it:

```sh
sudo apt-get update
sudo apt-get install -y openssh-server
sudo systemctl enable --now ssh
sudo sshd -t
```

If UFW is active and blocks SSH, request approval before allowing the named `OpenSSH` profile. Do not open unrelated ports. Preserve the existing `sshd_config` unless the user explicitly requests a compatible change.

## Verify both paths

1. Keep the KVM terminal open.
2. For UART, call `uart_status`, then `uart_read` with `cursor=0`. If `manual_terminal_connected=true`, do not mutate UART. Require a real login prompt and verify that it recovers after one adapter reconnect. Never send a password over UART.
3. For SSH, obtain the host address from `hostname -I`, then require the user to save the intended host and credential in device Settings. Use `ssh_exec` with a harmless command such as `printf 'EXOANCHOR_SSH_OK\n'; hostname`.
4. Treat service-active, port-listening, UART-ready, and network-reachable states as intermediate evidence, not end-to-end success.
5. Report which checks passed, which channel remains the recovery path, and any exact blocker. Roll back only changes made during this task, and never remove a pre-existing administrator configuration.
