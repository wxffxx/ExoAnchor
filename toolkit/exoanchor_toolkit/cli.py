from __future__ import annotations

import argparse
import json
import os
import shlex
import sys
from pathlib import Path

from . import __version__
from .discovery import (
    DISCOVERY_PORT,
    discover_devices,
    identify_uart_device,
)
from .errors import ToolkitError
from .firmware import (
    build_esptool_command,
    create_firmware_package,
    flash_firmware,
    load_firmware_package,
)
from .http_device import (
    DeviceHttpClient,
    HttpNetworkProvisioner,
    NetworkOtaUpdater,
    ensure_network_ota_allowed,
)
from .identity import (
    load_board_identity_credential,
    require_explicit_uart_flash_port,
    verify_package_profile,
)
from .network import NetworkProvisioner
from .network_configure import (
    NetworkConfiguration,
    VERIFY_TIMEOUT_SECONDS,
    configure_network,
)
from .ports import list_serial_ports, resolve_port
from .releases import (
    DEFAULT_GITHUB_REPOSITORY,
    DownloadedFirmware,
    download_firmware_release,
    download_latest_firmware,
    firmware_releases,
    open_firmware_package,
)
from .serial_cli import DiagnosticConsole


def _print_json(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True))


def _network_client(
    args: argparse.Namespace,
) -> NetworkProvisioner | HttpNetworkProvisioner:
    if args.transport == "network":
        return HttpNetworkProvisioner(
            DeviceHttpClient(
                args.device,
                username=args.username,
                password=_device_password(args),
                timeout=args.timeout,
            )
        )
    port = resolve_port(args.port)
    return NetworkProvisioner(
        DiagnosticConsole(port=port, baud=args.baud, timeout=args.timeout)
    )


def _device_password(args: argparse.Namespace) -> str:
    return (
        getattr(args, "password", None)
        or os.environ.get("EXOANCHOR_DEVICE_PASSWORD", "")
    )


def _run_followup(
    provisioner: NetworkProvisioner,
    args: argparse.Namespace,
    outputs: list[str],
) -> None:
    if getattr(args, "apply", False):
        outputs.append(provisioner.apply())
    if getattr(args, "commit", False):
        if not getattr(args, "apply", False):
            raise ToolkitError("--commit requires --apply")
        outputs.append(provisioner.commit())


def _command_ports(args: argparse.Namespace) -> int:
    ports = list_serial_ports()
    if args.json:
        _print_json([port.to_dict() for port in ports])
        return 0
    if not ports:
        print("No serial ports found.")
        return 0
    for port in ports:
        marker = "*" if port.likely_exoanchor else " "
        usb = (
            f"{port.vid:04x}:{port.pid:04x}"
            if port.vid is not None and port.pid is not None
            else "----:----"
        )
        print(f"{marker} {port.device:<32} {usb}  {port.description}")
    return 0


def _command_discover(args: argparse.Namespace) -> int:
    if args.transport == "uart":
        serial_port = resolve_port(args.serial_port)
        device = identify_uart_device(
            DiagnosticConsole(
                port=serial_port,
                baud=args.baud,
                timeout=args.timeout,
            )
        )
        devices: list[object] = [device]
    else:
        discovered = discover_devices(
            timeout=args.timeout,
            port=args.port,
            addresses=args.address,
            subnets=args.subnet,
        )
        devices = []
        for item in discovered:
            value = item.to_dict()
            value["transport"] = "network"
            devices.append(value)
    if args.json:
        _print_json(
            [
                device if isinstance(device, dict) else device.to_dict()
                for device in devices
            ]
        )
    elif devices:
        print(
            f"{'IP':<16} {'HOSTNAME':<24} {'DEVICE ID':<18} "
            f"{'BOARD':<28} {'FIRMWARE':<18} DEVICE LABEL"
        )
        for device in devices:
            value = (
                device
                if isinstance(device, dict)
                else device.to_dict()
            )
            print(
                f"{str(value.get('source_ip') or '-'):<16} "
                f"{str(value.get('hostname') or '-'):<24} "
                f"{str(value.get('device_id') or '-'):<18} "
                f"{str(value.get('board') or '-'):<28} "
                f"{str(value.get('firmware') or '-'):<18} "
                f"{value.get('device_label') or ''}"
            )
    else:
        print("No ExoAnchor devices found.")
    return 0 if devices else 1


def _command_network(args: argparse.Namespace) -> int:
    provisioner = _network_client(args)
    command = args.network_command
    if command == "show":
        snapshot = provisioner.show()
        if args.json:
            _print_json(snapshot.to_dict())
        else:
            print(snapshot.raw)
        return 0

    if command == "configure":
        client = (
            provisioner.client
            if isinstance(provisioner, HttpNetworkProvisioner)
            else None
        )
        target = (
            client.target.base_url
            if client is not None
            else provisioner.console.port
        )
        configured = configure_network(
            provisioner,
            NetworkConfiguration(
                mode=args.mode,
                hostname=args.hostname or "",
                address=args.address or "",
                netmask=args.netmask or "",
                gateway=args.gateway or "-",
                dns_primary=args.dns_primary or "-",
                dns_secondary=args.dns_secondary or "-",
            ),
            transport=args.transport,
            target=target,
            client=client,
            discovery_subnets=tuple(args.discovery_subnet or ()),
            verify_timeout=args.verify_timeout,
        )
        if args.json:
            _print_json(configured.to_dict())
        else:
            print("\n".join(configured.output))
            print(
                "Network configuration verified and saved: "
                + configured.target
            )
        return 0

    outputs: list[str] = []
    if command == "dhcp":
        outputs.append(provisioner.stage_dhcp(args.hostname))
        _run_followup(provisioner, args, outputs)
    elif command == "static":
        outputs.append(
            provisioner.stage_static(
                args.address,
                args.netmask,
                args.gateway,
                args.dns_primary,
                args.dns_secondary,
            )
        )
        _run_followup(provisioner, args, outputs)
    elif command == "hostname":
        outputs.append(provisioner.stage_hostname(args.hostname))
        _run_followup(provisioner, args, outputs)
    elif command == "apply":
        outputs.append(provisioner.apply())
    elif command == "commit":
        outputs.append(provisioner.commit())
    elif command == "rollback":
        outputs.append(provisioner.rollback())
    elif command == "reset":
        if not args.yes:
            raise ToolkitError("network reset requires --yes")
        outputs.append(provisioner.reset())
    else:  # pragma: no cover - argparse prevents this.
        raise ToolkitError(f"unknown network command: {command}")

    if args.json:
        target = (
            provisioner.console.port
            if isinstance(provisioner, NetworkProvisioner)
            else provisioner.client.target.base_url
        )
        _print_json(
            {
                "transport": args.transport,
                "target": target,
                "output": outputs,
            }
        )
    else:
        print("\n".join(outputs))
    return 0


def _command_firmware(args: argparse.Namespace) -> int:
    command = args.firmware_command
    if command == "versions":
        releases = firmware_releases(
            args.repository,
            include_prerelease=not args.stable_only,
        )
        if args.json:
            _print_json([release.to_dict() for release in releases])
        else:
            print(f"{'TAG':<32} {'TYPE':<12} {'PUBLISHED':<20} NAME")
            for release in releases:
                release_type = "prerelease" if release.prerelease else "stable"
                published = release.published_at.replace("T", " ")[:19]
                print(
                    f"{release.tag:<32} {release_type:<12} "
                    f"{published:<20} {release.name}"
                )
        return 0

    if command == "download":
        downloaded = download_firmware_release(
            args.repository,
            tag=args.tag,
            output_dir=args.output,
            include_prerelease=not args.stable_only,
        )
        return _show_downloaded_firmware(downloaded, as_json=args.json)

    if command == "latest":
        downloaded = download_latest_firmware(
            args.repository,
            output_dir=args.output,
            include_prerelease=not args.stable_only,
        )
        return _show_downloaded_firmware(downloaded, as_json=args.json)

    if command == "package":
        manifest = create_firmware_package(args.build_dir, args.output)
        package = load_firmware_package(manifest)
        if args.json:
            _print_json(package.to_dict())
        else:
            print(f"Created and verified: {manifest}")
        return 0

    with open_firmware_package(args.package) as package:
        if command in {"inspect", "verify"}:
            if args.json:
                _print_json(package.to_dict())
            else:
                print(
                    f"ExoAnchor {package.board} {package.version}\n"
                    f"Chip: {package.chip}; flash: {package.flash_size} "
                    f"{package.flash_mode}/{package.flash_frequency}\n"
                    f"Manifest: {package.manifest_path}"
                )
                for segment in package.segments:
                    print(
                        f"  0x{segment.address:06x}  {segment.size:8d}  "
                        f"{segment.sha256[:12]}…  {segment.relative_path}"
                    )
                print("Verification: PASS")
            return 0

        if command == "flash":
            if args.transport == "uart":
                target = require_explicit_uart_flash_port(args.port)
                credential = load_board_identity_credential(
                    args.identity_registry,
                    args.board_record,
                )
                verify_package_profile(package.board, credential)
                command_line = build_esptool_command(
                    package, target, args.baud
                )
            else:
                ensure_network_ota_allowed(package)
                client = DeviceHttpClient(
                    args.device,
                    username=args.username,
                    password=_device_password(args),
                    timeout=args.timeout,
                )
                target = client.target.base_url
                command_line = [
                    "network-ota",
                    target,
                    str(package.manifest_path),
                ]
            if args.dry_run:
                if args.json:
                    _print_json(
                        {
                            "package": package.to_dict(),
                            "transport": args.transport,
                            "target": target,
                            "command": command_line,
                        }
                    )
                else:
                    print(shlex.join(command_line))
                return 0
            if not args.yes:
                if not sys.stdin.isatty():
                    raise ToolkitError("non-interactive flashing requires --yes")
                expected = f"FLASH {package.board}"
                print(
                    f"About to update {package.version} for {package.board} "
                    f"through {args.transport} at {target}.\n"
                    + (
                        "Toolkit will not issue a full-chip erase; only package "
                        "segments will be written."
                        if args.transport == "uart"
                        else "Toolkit will upload the verified application image "
                        "to the inactive OTA partition."
                    )
                )
                entered = input(f"Type `{expected}` to continue: ").strip()
                if entered != expected:
                    raise ToolkitError("flash cancelled")
            if args.transport == "uart":
                flash_firmware(
                    package,
                    target,
                    args.baud,
                    identity_registry=args.identity_registry,
                    board_record=args.board_record,
                )
            else:
                for line in NetworkOtaUpdater(client).update(
                    package,
                    reboot=not args.no_reboot,
                ):
                    print(line)
            return 0

    raise ToolkitError(f"unknown firmware command: {command}")


def _show_downloaded_firmware(
    downloaded: DownloadedFirmware,
    *,
    as_json: bool,
) -> int:
    if as_json:
        _print_json(downloaded.to_dict())
    else:
        source = "cache" if downloaded.cached else "GitHub"
        print(
            f"ExoAnchor {downloaded.package.board} "
            f"{downloaded.package.version}\n"
            f"Release: {downloaded.release.tag} ({source})\n"
            f"Package: {downloaded.directory}\n"
            "Verification: PASS"
        )
    return 0


def _add_serial_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--port",
        default="auto",
        help="UART0 serial port or 'auto' when exactly one device is present",
    )
    parser.add_argument("--baud", type=int, default=115200, help="UART0 baud")
    parser.add_argument(
        "--timeout", type=float, default=5.0, help="UART command timeout in seconds"
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")


def _add_transport_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--transport",
        choices=("network", "uart"),
        default="uart",
        help="control the device through its HTTP API or diagnostic UART0",
    )
    parser.add_argument(
        "--device",
        default="",
        help="device IPv4 address, optionally with an HTTP port",
    )
    parser.add_argument(
        "--username",
        default="admin",
        help="device web account username for network transport",
    )
    parser.add_argument(
        "--password",
        help=(
            "device web account password; alternatively set "
            "EXOANCHOR_DEVICE_PASSWORD"
        ),
    )


def _add_network_child_options(parser: argparse.ArgumentParser) -> None:
    """Accept transport options after the network subcommand as well.

    The top-level network parser keeps the original
    ``network --transport ... show`` form compatible.  Suppressed defaults on
    the child parser prevent absent child options from overwriting values that
    were supplied before the subcommand.
    """

    parser.add_argument(
        "--port",
        default=argparse.SUPPRESS,
        help="UART0 serial port or 'auto'",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=argparse.SUPPRESS,
        help="UART0 baud",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=argparse.SUPPRESS,
        help="UART or device HTTP timeout in seconds",
    )
    parser.add_argument(
        "--transport",
        choices=("network", "uart"),
        default=argparse.SUPPRESS,
        help="operate through the device HTTP API or diagnostic UART0",
    )
    parser.add_argument(
        "--device",
        default=argparse.SUPPRESS,
        help="device IPv4 address, optionally with an HTTP port",
    )
    parser.add_argument(
        "--username",
        default=argparse.SUPPRESS,
        help="device web account username for network transport",
    )
    parser.add_argument(
        "--password",
        default=argparse.SUPPRESS,
        help=(
            "device web account password; alternatively set "
            "EXOANCHOR_DEVICE_PASSWORD"
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        default=argparse.SUPPRESS,
        help="emit machine-readable JSON",
    )


def _add_apply_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--apply",
        action="store_true",
        help="apply the staged configuration with the device rollback timer",
    )
    parser.add_argument(
        "--commit",
        action="store_true",
        help="commit immediately after apply; requires --apply",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="exoanchor-toolkit",
        description="Provision ExoAnchor networking and flash verified firmware packages.",
    )
    parser.add_argument("--version", action="version", version=__version__)
    commands = parser.add_subparsers(dest="command", required=True)

    ports = commands.add_parser("ports", help="list serial ports")
    ports.add_argument("--json", action="store_true")
    ports.set_defaults(handler=_command_ports)

    discover = commands.add_parser(
        "discover", help="find ExoAnchor devices on the local IPv4 network"
    )
    discover.add_argument(
        "--timeout", type=float, default=2.0, help="response window in seconds"
    )
    discover.add_argument(
        "--transport",
        choices=("network", "uart"),
        default="network",
        help="discover by LAN broadcast or identify the UART0-connected device",
    )
    discover.add_argument(
        "--port", type=int, default=DISCOVERY_PORT, help="discovery UDP port"
    )
    discover.add_argument(
        "--address",
        action="append",
        help="broadcast or unicast destination; may be repeated",
    )
    discover.add_argument(
        "--subnet",
        action="append",
        help="also probe every host in an IPv4 CIDR, limited to 1024 hosts",
    )
    discover.add_argument(
        "--serial-port",
        default="auto",
        help="diagnostic UART0 port for --transport uart",
    )
    discover.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="diagnostic UART0 baud for --transport uart",
    )
    discover.add_argument("--json", action="store_true")
    discover.set_defaults(handler=_command_discover)

    network = commands.add_parser(
        "network", help="manage Ethernet configuration through network or UART0"
    )
    _add_serial_options(network)
    _add_transport_options(network)
    network_commands = network.add_subparsers(dest="network_command", required=True)

    show = network_commands.add_parser(
        "show", help="show runtime and stored configuration"
    )
    _add_network_child_options(show)

    configure = network_commands.add_parser(
        "configure",
        help="stage, apply, verify, and safely commit one configuration",
    )
    _add_network_child_options(configure)
    configure.add_argument(
        "--mode",
        choices=("dhcp", "static"),
        required=True,
        help="IPv4 address mode",
    )
    configure.add_argument("--hostname")
    configure.add_argument("--address")
    configure.add_argument("--netmask")
    configure.add_argument("--gateway", default="-")
    configure.add_argument("--dns-primary", default="-")
    configure.add_argument("--dns-secondary", default="-")
    configure.add_argument(
        "--discovery-subnet",
        action="append",
        help=(
            "fallback IPv4 CIDR for DHCP address rediscovery; "
            "may be repeated"
        ),
    )
    configure.add_argument(
        "--verify-timeout",
        type=float,
        default=VERIFY_TIMEOUT_SECONDS,
        help=(
            "seconds to verify the pending configuration before leaving "
            "it to roll back"
        ),
    )

    dhcp = network_commands.add_parser("dhcp", help="stage DHCP + AutoIP")
    _add_network_child_options(dhcp)
    dhcp.add_argument("--hostname")
    _add_apply_options(dhcp)

    static = network_commands.add_parser("static", help="stage static IPv4")
    _add_network_child_options(static)
    static.add_argument("address")
    static.add_argument("netmask")
    static.add_argument("--gateway", default="-")
    static.add_argument("--dns-primary", default="-")
    static.add_argument("--dns-secondary", default="-")
    _add_apply_options(static)

    hostname = network_commands.add_parser(
        "hostname", help="stage a hostname without changing address mode"
    )
    _add_network_child_options(hostname)
    hostname.add_argument("hostname")
    _add_apply_options(hostname)

    apply = network_commands.add_parser(
        "apply", help="apply staged config for 120 seconds"
    )
    _add_network_child_options(apply)
    commit = network_commands.add_parser(
        "commit", help="confirm pending config as last-good"
    )
    _add_network_child_options(commit)
    rollback = network_commands.add_parser(
        "rollback", help="restore last-good config"
    )
    _add_network_child_options(rollback)
    reset = network_commands.add_parser("reset", help="restore factory DHCP configuration")
    _add_network_child_options(reset)
    reset.add_argument("--yes", action="store_true", help="confirm network reset")
    network.set_defaults(handler=_command_network)

    firmware = commands.add_parser(
        "firmware", help="package, verify, inspect, or flash precompiled firmware"
    )
    firmware_commands = firmware.add_subparsers(
        dest="firmware_command", required=True
    )

    versions = firmware_commands.add_parser(
        "versions",
        help="list verified firmware package releases available on GitHub",
    )
    versions.add_argument(
        "--repository",
        default=DEFAULT_GITHUB_REPOSITORY,
        help="GitHub repository in owner/name form",
    )
    versions.add_argument(
        "--stable-only",
        action="store_true",
        help="ignore prerelease firmware",
    )
    versions.add_argument("--json", action="store_true")

    download = firmware_commands.add_parser(
        "download",
        help="download and verify a firmware package by release tag",
    )
    download.add_argument("tag", help="exact GitHub Release tag")
    download.add_argument(
        "--repository",
        default=DEFAULT_GITHUB_REPOSITORY,
        help="GitHub repository in owner/name form",
    )
    download.add_argument(
        "--output",
        type=Path,
        help="parent directory for the verified package; defaults to the user cache",
    )
    download.add_argument(
        "--stable-only",
        action="store_true",
        help="reject prerelease firmware",
    )
    download.add_argument("--json", action="store_true")

    latest = firmware_commands.add_parser(
        "latest",
        help="download and verify the newest firmware package from GitHub Releases",
    )
    latest.add_argument(
        "--repository",
        default=DEFAULT_GITHUB_REPOSITORY,
        help="GitHub repository in owner/name form",
    )
    latest.add_argument(
        "--output",
        type=Path,
        help="parent directory for the verified package; defaults to the user cache",
    )
    latest.add_argument(
        "--stable-only",
        action="store_true",
        help="ignore prerelease firmware",
    )
    latest.add_argument("--json", action="store_true")

    package = firmware_commands.add_parser(
        "package", help="create a verified package from an ESP-IDF build directory"
    )
    package.add_argument("build_dir", type=Path)
    package.add_argument("output", type=Path)
    package.add_argument("--json", action="store_true")

    for name in ("inspect", "verify"):
        inspect = firmware_commands.add_parser(name, help=f"{name} a firmware package")
        inspect.add_argument("package", type=Path)
        inspect.add_argument("--json", action="store_true")

    flash = firmware_commands.add_parser("flash", help="flash a verified package")
    flash.add_argument("package", type=Path)
    flash.add_argument("--port", default="auto")
    flash.add_argument("--baud", type=int, default=460800)
    flash.add_argument(
        "--identity-registry",
        type=Path,
        help="authoritative BOARD_IDENTITY_REGISTRY.toml (required for UART)",
    )
    flash.add_argument(
        "--board-record",
        help="exact registry record for the operator-confirmed physical model",
    )
    _add_transport_options(flash)
    flash.add_argument(
        "--timeout",
        type=float,
        default=180.0,
        help="device HTTP timeout for network OTA",
    )
    flash.add_argument(
        "--no-reboot",
        action="store_true",
        help="upload network OTA without rebooting into the new firmware",
    )
    flash.add_argument("--yes", action="store_true", help="skip interactive confirmation")
    flash.add_argument(
        "--dry-run", action="store_true", help="verify and print the esptool command"
    )
    flash.add_argument("--json", action="store_true")
    firmware.set_defaults(handler=_command_firmware)

    gui = commands.add_parser(
        "gui", help="open the lightweight local browser interface"
    )
    gui.add_argument(
        "--port",
        type=int,
        default=0,
        help="local HTTP port; defaults to an available ephemeral port",
    )
    gui.add_argument(
        "--no-open",
        action="store_true",
        help="start the interface without opening a browser",
    )
    gui.set_defaults(handler=_command_gui)
    return parser


def _command_gui(args: argparse.Namespace) -> int:
    from .gui import launch_gui

    launch_gui(port=args.port, open_browser=not args.no_open)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except ToolkitError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("cancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
