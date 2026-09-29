---
name: bridge-uart-ssh-access
description: Use an existing authenticated UART or SSH maintenance path to establish, recover, and verify the other path on an Ubuntu target host. Use for SSH-to-UART, UART-to-SSH, serial-console fallback, second-channel recovery, or requests to configure UART and SSH from one another.
---

# Bridge UART and SSH Access

## Invariants

1. Preserve the currently working channel until the second channel passes an independent end-to-end test.
2. Call the source-channel status tool before mutation. Never infer that UART is unavailable because SSH is unconfigured, or vice versa.
3. If `manual_terminal_connected=true`, keep the manual Terminal attached as an output observer. During each Agent/MCP mutation, let terminal control temporarily disable manual input, publish the automation label/input/output, and honor the browser stop action.
4. Never send passwords, private keys, API keys, or tokens through UART or model-visible command text. Use device-local SSH credentials and ask the human to authenticate an interactive UART shell.
5. Do not weaken SSH security, enable root login, create passwordless sudo, or alter unrelated firewall rules.
6. This skill does not bypass tool policy, Request Broker approval, manual ownership, host authentication, or destructive-action safeguards.

## Choose the source path

- If SSH works, use `ssh_exec` to configure UART, then verify with `uart_status` and `uart_read`.
- If an authenticated UART shell works, use bounded `uart_write` commands to configure SSH, then verify with `ssh_exec`.
- If neither path is authenticated, stop and ask the user to restore KVM or manually authenticate one path.
- If both paths work, inspect and report their state before making any change.

## Use SSH to establish UART

Use `ssh_exec` for bounded, observable steps. Resolve the stable adapter identity first:

```sh
ls -l /dev/serial/by-id/
readlink -f /dev/serial/by-id/<adapter-id>
systemctl status dev-ttyACM0.device --no-pager
```

Replace `ttyACM0` only after confirming the resolved basename. Preserve any pre-existing drop-in before changing it. Configure this service contract:

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

Apply and inspect:

```sh
sudo install -d -m 0755 /etc/systemd/system/serial-getty@ttyACM0.service.d
sudo systemctl daemon-reload
sudo systemctl reset-failed serial-getty@ttyACM0.service
sudo systemctl enable --now serial-getty@ttyACM0.service
systemctl cat serial-getty@ttyACM0.service
systemctl status serial-getty@ttyACM0.service --no-pager
journalctl -u serial-getty@ttyACM0.service -n 50 --no-pager
```

Then call `uart_status` and `uart_read` with `cursor=0`. Require a real login prompt and one reconnect recovery. Do not use `uart_write` to submit credentials.

## Use UART to establish SSH

1. Call `uart_status`, then `uart_read` with `cursor=0`.
2. Require evidence of an authenticated shell prompt. If only a login prompt is visible, ask the human to authenticate in the Terminal page.
3. Send one command per `uart_write` with `append_enter=true`. Continue `uart_read` from `next_cursor` until `pending_bytes=0` before sending the next command.
4. Inspect SSH:

```sh
command -v sshd
systemctl status ssh --no-pager
sudo sshd -t
ss -lnt | grep -E ':[2]2[[:space:]]'
hostname -I
```

5. If OpenSSH Server is missing, obtain approval before running:

```sh
sudo apt-get update
sudo apt-get install -y openssh-server
sudo systemctl enable --now ssh
sudo sshd -t
```

Never transmit a sudo password with `uart_write`. For the external managed MCP path, use `exoanchor_ssh_bootstrap_from_uart`: it accepts no host override or password, derives the target address from this UART probe, and asks firmware to copy `console://default` to the SSH secret store locally. If UFW is active and blocks SSH, request approval before allowing only the `OpenSSH` profile.

After local credential binding, run `printf 'EXOANCHOR_SSH_OK\n'; hostname; id -un` through `ssh_exec`. Require the UART-observed address, SSH username, marker, zero exit status, and—when requested—`systemctl is-enabled ssh=enabled`. Do not treat a listening port alone as success.

## Complete or recover

Report the source channel, changes made, exact UART and SSH verification evidence, and the retained recovery path. If the new channel fails, keep the source channel intact, inspect service status and bounded logs, and roll back only task-owned changes. Never delete an unknown pre-existing configuration.
