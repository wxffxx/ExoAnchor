# Local Access Knowledge

This project currently targets:

- IP: `192.168.1.67`
- SSH user: `wxffxx`
- Current SSH / sudo password: `123898`

For ExoAnchor SSH automation:

- Plain `sudo ...` is not enough, because there is no interactive password prompt.
- Use non-interactive sudo in shell commands:

```bash
printf '%s\n' '123898' | sudo -S <command>
```

Example:

```bash
printf '%s\n' '123898' | sudo -S apt-get update
```

If the password changes later, update `exoanchor_config.json` and this knowledge note together.
