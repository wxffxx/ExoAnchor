from __future__ import annotations

import hashlib
import importlib.metadata
import importlib.util
import json
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .errors import ToolkitError
from .identity import (
    load_board_identity_credential,
    probe_chip_identity,
    require_explicit_uart_flash_port,
    verify_ch343_identity,
    verify_chip_identity,
    verify_package_profile,
)


MANIFEST_NAME = "exoanchor-firmware.json"
THIRD_PARTY_NOTICES_NAME = "THIRD_PARTY_NOTICES.txt"
SCHEMA_VERSION = 1
PRODUCT_ID = "exoanchor-esp32p4"
REQUIRED_OFFSETS = {0x2000, 0x8000, 0xF000, 0x20000}
FLASH_SIZES = {
    "1MB": 1 * 1024 * 1024,
    "2MB": 2 * 1024 * 1024,
    "4MB": 4 * 1024 * 1024,
    "8MB": 8 * 1024 * 1024,
    "16MB": 16 * 1024 * 1024,
    "32MB": 32 * 1024 * 1024,
    "64MB": 64 * 1024 * 1024,
    "128MB": 128 * 1024 * 1024,
}


@dataclass(frozen=True)
class FirmwareSegment:
    address: int
    path: Path
    relative_path: str
    size: int
    sha256: str

    def to_dict(self) -> dict[str, object]:
        return {
            "address": f"0x{self.address:x}",
            "file": self.relative_path,
            "size": self.size,
            "sha256": self.sha256,
        }


@dataclass(frozen=True)
class FirmwarePackage:
    root: Path
    manifest_path: Path
    version: str
    board: str
    chip: str
    flash_size: str
    flash_mode: str
    flash_frequency: str
    before: str
    after: str
    use_stub: bool
    segments: tuple[FirmwareSegment, ...]

    def to_dict(self) -> dict[str, object]:
        return {
            "manifest": str(self.manifest_path),
            "product": PRODUCT_ID,
            "version": self.version,
            "board": self.board,
            "chip": self.chip,
            "flash_size": self.flash_size,
            "flash_mode": self.flash_mode,
            "flash_frequency": self.flash_frequency,
            "segments": [segment.to_dict() for segment in self.segments],
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _parse_address(value: object) -> int:
    if isinstance(value, bool):
        raise ToolkitError("firmware segment address cannot be boolean")
    try:
        address = int(str(value), 0)
    except (TypeError, ValueError) as exc:
        raise ToolkitError(f"invalid firmware segment address: {value}") from exc
    if address < 0 or address % 0x1000 != 0:
        raise ToolkitError(
            f"firmware segment address must be non-negative and 4K aligned: {value}"
        )
    return address


def _read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ToolkitError(f"missing firmware manifest: {path}") from exc
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ToolkitError(f"cannot read firmware manifest {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ToolkitError("firmware manifest root must be an object")
    return value


def _manifest_path(package: str | Path) -> Path:
    path = Path(package).expanduser()
    if path.is_dir():
        path = path / MANIFEST_NAME
    return path.resolve()


def load_firmware_package(package: str | Path) -> FirmwarePackage:
    manifest_path = _manifest_path(package)
    raw = _read_json(manifest_path)
    if raw.get("schema_version") != SCHEMA_VERSION:
        raise ToolkitError(
            f"unsupported firmware manifest schema: {raw.get('schema_version')}"
        )
    if raw.get("product") != PRODUCT_ID:
        raise ToolkitError(f"not an ExoAnchor ESP32-P4 package: {raw.get('product')}")

    firmware = raw.get("firmware")
    flash = raw.get("flash")
    segments_raw = raw.get("segments")
    if not isinstance(firmware, dict) or not isinstance(flash, dict):
        raise ToolkitError("manifest must contain firmware and flash objects")
    if not isinstance(segments_raw, list) or not segments_raw:
        raise ToolkitError("manifest must contain at least one firmware segment")

    version = str(firmware.get("version", "")).strip()
    board = str(firmware.get("board", "")).strip()
    chip = str(firmware.get("chip", "")).strip().lower()
    flash_size = str(firmware.get("flash_size", "")).strip()
    if not version or not board:
        raise ToolkitError("firmware version and board must be non-empty")
    if chip != "esp32p4":
        raise ToolkitError(f"unsupported target chip: {chip}")
    if flash_size not in FLASH_SIZES:
        raise ToolkitError(f"unsupported flash size: {flash_size}")

    flash_mode = str(flash.get("mode", "")).strip()
    flash_frequency = str(flash.get("frequency", "")).strip()
    before = str(flash.get("before", "default_reset")).strip()
    after = str(flash.get("after", "hard_reset")).strip()
    use_stub = bool(flash.get("stub", True))
    if flash_mode not in {"dio", "dout", "qio", "qout"}:
        raise ToolkitError(f"unsupported flash mode: {flash_mode}")
    if flash_frequency not in {
        "80m",
        "60m",
        "48m",
        "40m",
        "30m",
        "26m",
        "24m",
        "20m",
        "16m",
        "15m",
        "12m",
    }:
        raise ToolkitError(f"unsupported flash frequency: {flash_frequency}")
    if before not in {"default_reset", "usb_reset", "no_reset", "no_reset_no_sync"}:
        raise ToolkitError(f"unsupported reset-before mode: {before}")
    if after not in {"hard_reset", "soft_reset", "no_reset", "no_reset_stub"}:
        raise ToolkitError(f"unsupported reset-after mode: {after}")

    root = manifest_path.parent.resolve()
    segments: list[FirmwareSegment] = []
    seen_addresses: set[int] = set()
    for item in segments_raw:
        if not isinstance(item, dict):
            raise ToolkitError("each firmware segment must be an object")
        address = _parse_address(item.get("address"))
        if address in seen_addresses:
            raise ToolkitError(f"duplicate firmware address: 0x{address:x}")
        seen_addresses.add(address)

        relative = str(item.get("file", "")).strip()
        if not relative:
            raise ToolkitError(f"segment 0x{address:x} has no file")
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise ToolkitError(f"unsafe firmware file path: {relative}")
        path = (root / relative_path).resolve()
        if not path.is_relative_to(root):
            raise ToolkitError(f"firmware file escapes package: {relative}")
        try:
            actual_size = path.stat().st_size
        except OSError as exc:
            raise ToolkitError(f"cannot read firmware segment {path}: {exc}") from exc
        if not path.is_file() or actual_size <= 0:
            raise ToolkitError(f"firmware segment is empty or not a file: {path}")

        try:
            expected_size = int(item.get("size"))
        except (TypeError, ValueError) as exc:
            raise ToolkitError(f"invalid size for segment 0x{address:x}") from exc
        expected_hash = str(item.get("sha256", "")).lower()
        if expected_size != actual_size:
            raise ToolkitError(
                f"size mismatch for {relative}: expected {expected_size}, "
                f"found {actual_size}"
            )
        if len(expected_hash) != 64 or any(
            ch not in "0123456789abcdef" for ch in expected_hash
        ):
            raise ToolkitError(f"invalid SHA-256 for {relative}")
        actual_hash = sha256_file(path)
        if actual_hash != expected_hash:
            raise ToolkitError(
                f"SHA-256 mismatch for {relative}: expected {expected_hash}, "
                f"found {actual_hash}"
            )
        segments.append(
            FirmwareSegment(
                address=address,
                path=path,
                relative_path=relative_path.as_posix(),
                size=actual_size,
                sha256=actual_hash,
            )
        )

    if seen_addresses != REQUIRED_OFFSETS:
        missing = REQUIRED_OFFSETS - seen_addresses
        extra = seen_addresses - REQUIRED_OFFSETS
        details = []
        if missing:
            details.append(
                "missing " + ", ".join(f"0x{address:x}" for address in sorted(missing))
            )
        if extra:
            details.append(
                "unexpected "
                + ", ".join(f"0x{address:x}" for address in sorted(extra))
            )
        raise ToolkitError("unsafe ExoAnchor flash layout: " + "; ".join(details))

    segments.sort(key=lambda item: item.address)
    flash_bytes = FLASH_SIZES[flash_size]
    for index, segment in enumerate(segments):
        end = segment.address + segment.size
        if end > flash_bytes:
            raise ToolkitError(
                f"segment {segment.relative_path} exceeds {flash_size} flash"
            )
        if index + 1 < len(segments) and end > segments[index + 1].address:
            raise ToolkitError(
                f"segment overlap at 0x{segments[index + 1].address:x}"
            )

    return FirmwarePackage(
        root=root,
        manifest_path=manifest_path,
        version=version,
        board=board,
        chip=chip,
        flash_size=flash_size,
        flash_mode=flash_mode,
        flash_frequency=flash_frequency,
        before=before,
        after=after,
        use_stub=use_stub,
        segments=tuple(segments),
    )


def _read_sdkconfig_value(path: Path, key: str) -> str | None:
    prefix = key + "="
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ToolkitError(f"cannot read {path}: {exc}") from exc
    for line in lines:
        if line.startswith(prefix):
            return line[len(prefix) :].strip().strip('"')
    return None


def create_firmware_package(build_dir: str | Path, output_dir: str | Path) -> Path:
    build = Path(build_dir).expanduser().resolve()
    output = Path(output_dir).expanduser().resolve()
    if output.exists():
        raise ToolkitError(f"output path already exists; refusing to overwrite: {output}")

    flasher_path = build / "flasher_args.json"
    project_path = build / "project_description.json"
    sdkconfig_path = build / "sdkconfig"
    notices_path = build / THIRD_PARTY_NOTICES_NAME
    flasher = _read_json(flasher_path)
    project = _read_json(project_path)
    flash_files = flasher.get("flash_files")
    settings = flasher.get("flash_settings")
    extra = flasher.get("extra_esptool_args")
    if (
        not isinstance(flash_files, dict)
        or not isinstance(settings, dict)
        or not isinstance(extra, dict)
    ):
        raise ToolkitError("ESP-IDF flasher_args.json is incomplete")

    chip = str(project.get("target", "")).lower()
    version = str(project.get("project_version", "")).strip()
    board = _read_sdkconfig_value(sdkconfig_path, "CONFIG_SI_BOARD_ID")
    flash_size = str(settings.get("flash_size", "")).strip()
    if chip != "esp32p4" or not version or not board:
        raise ToolkitError(
            "build metadata must identify ESP32-P4, firmware version, and board"
        )
    if not notices_path.is_file():
        raise ToolkitError(
            f"build is missing required third-party notices: {notices_path}"
        )

    normalized: list[tuple[int, Path]] = []
    for address_text, relative_text in flash_files.items():
        address = _parse_address(address_text)
        source = (build / str(relative_text)).resolve()
        if not source.is_relative_to(build) or not source.is_file():
            raise ToolkitError(f"unsafe or missing build artifact: {relative_text}")
        normalized.append((address, source))
    if {address for address, _ in normalized} != REQUIRED_OFFSETS:
        raise ToolkitError("build does not contain the complete ExoAnchor flash layout")

    output.mkdir(parents=True)
    shutil.copyfile(notices_path, output / THIRD_PARTY_NOTICES_NAME)
    image_dir = output / "images"
    image_dir.mkdir()
    segments = []
    for address, source in sorted(normalized):
        destination_name = f"{address:06x}-{source.name}"
        destination = image_dir / destination_name
        shutil.copyfile(source, destination)
        segments.append(
            {
                "address": f"0x{address:x}",
                "file": destination.relative_to(output).as_posix(),
                "size": destination.stat().st_size,
                "sha256": sha256_file(destination),
            }
        )

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "product": PRODUCT_ID,
        "firmware": {
            "version": version,
            "board": board,
            "chip": chip,
            "flash_size": flash_size,
        },
        "flash": {
            "mode": str(settings.get("flash_mode", "")),
            "frequency": str(settings.get("flash_freq", "")),
            "before": str(extra.get("before", "default_reset")),
            "after": str(extra.get("after", "hard_reset")),
            "stub": bool(extra.get("stub", True)),
        },
        "segments": segments,
    }
    manifest_path = output / MANIFEST_NAME
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    load_firmware_package(manifest_path)
    return manifest_path


def create_firmware_archive(
    package: str | Path,
    output_path: str | Path,
) -> Path:
    verified = load_firmware_package(package)
    output = Path(output_path).expanduser().resolve()
    if output.exists():
        raise ToolkitError(f"output path already exists; refusing to overwrite: {output}")
    if output.suffix.lower() != ".zip":
        raise ToolkitError("firmware release archive must use a .zip extension")
    output.parent.mkdir(parents=True, exist_ok=True)
    notices_path = verified.root / THIRD_PARTY_NOTICES_NAME
    if not notices_path.is_file():
        raise ToolkitError(
            f"firmware package is missing third-party notices: {notices_path}"
        )
    files = [
        (verified.manifest_path, MANIFEST_NAME),
        (notices_path, THIRD_PARTY_NOTICES_NAME),
    ]
    files.extend((segment.path, segment.relative_path) for segment in verified.segments)
    try:
        with zipfile.ZipFile(
            output,
            mode="x",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
        ) as archive:
            for source, relative in files:
                info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, source.read_bytes())
    except OSError as exc:
        raise ToolkitError(f"cannot create firmware release archive: {exc}") from exc
    return output


def build_esptool_command(
    package: FirmwarePackage,
    port: str,
    baud: int,
    *,
    esptool_major: int | None = None,
) -> list[str]:
    port = require_explicit_uart_flash_port(port)
    if baud <= 0:
        raise ToolkitError("flash baud must be positive")
    if esptool_major is None:
        try:
            esptool_major = int(
                importlib.metadata.version("esptool").split(".", 1)[0]
            )
        except (importlib.metadata.PackageNotFoundError, ValueError):
            esptool_major = 4
    modern_cli = esptool_major >= 5
    before = package.before.replace("_", "-") if modern_cli else package.before
    after = package.after.replace("_", "-") if modern_cli else package.after
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        package.chip,
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        before,
        "--after",
        after,
    ]
    if not package.use_stub:
        command.append("--no-stub")
    command.append("write-flash" if modern_cli else "write_flash")
    if not modern_cli:
        command.append("--verify")
    command.extend(
        [
            "--flash-mode" if modern_cli else "--flash_mode",
            package.flash_mode,
            "--flash-freq" if modern_cli else "--flash_freq",
            package.flash_frequency,
            "--flash-size" if modern_cli else "--flash_size",
            package.flash_size,
        ]
    )
    for segment in package.segments:
        command.extend([f"0x{segment.address:x}", str(segment.path)])
    return command


def flash_firmware(
    package: FirmwarePackage,
    port: str,
    baud: int = 460800,
    output: Callable[[str], None] | None = None,
    *,
    identity_registry: str | Path | None = None,
    board_record: str | None = None,
    verify_runtime: bool = True,
    runtime_baud: int = 115200,
    verification_timeout: float = 30.0,
) -> dict[str, object] | None:
    port = require_explicit_uart_flash_port(port)
    credential = load_board_identity_credential(
        identity_registry,
        board_record,
    )
    verify_package_profile(package.board, credential)
    from .ports import capture_port_identity

    identity = capture_port_identity(port)
    verify_ch343_identity(identity, credential)
    if importlib.util.find_spec("esptool") is None:
        raise ToolkitError(
            "esptool is required; install the toolkit with "
            "`python -m pip install ./toolkit`"
        )
    observed_identity = probe_chip_identity(port)
    verify_chip_identity(observed_identity, credential)
    write = output or (lambda line: print(line, flush=True))
    command = build_esptool_command(package, port, baud)
    write(
        f"Verified {credential.physical_model}, CH343 "
        f"{credential.ch343_serial}, eFuse {credential.efuse_base_mac}, "
        f"revision {credential.silicon_revision}, and profile "
        f"{package.board}; flashing {port}"
    )
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as exc:
        raise ToolkitError(f"cannot start esptool: {exc}") from exc
    assert process.stdout is not None
    for line in process.stdout:
        write(line.rstrip())
    return_code = process.wait()
    if return_code != 0:
        raise ToolkitError(f"esptool failed with exit code {return_code}")
    if not verify_runtime:
        return None

    from .uart_recovery import wait_for_uart_runtime

    write("Flash write completed; waiting for the same device runtime")
    verification = wait_for_uart_runtime(
        identity,
        expected_version=package.version,
        expected_board=package.board,
        baud=runtime_baud,
        timeout=verification_timeout,
    )
    write(
        "UART0 runtime verified: "
        f"{verification['board']} {verification['firmware']} on "
        f"{verification['port']}"
    )
    return verification
