#!/usr/bin/env python3
"""Fail when publishable repository files contain private environment evidence."""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


MAX_TEXT_BYTES = 2 * 1024 * 1024
SELF_PATH = Path("scripts/check-public-privacy.py")
SAFE_USB_SUFFIXES = {"DEVICE", "EXACT"}
SAFE_DEVICE_SUFFIXES = {"000001", "123456"}

MAC_RE = re.compile(r"(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b")
MAC_BYTES_RE = re.compile(
    r"(?i)0x(?P<first>[0-9a-f]{2})"
    r"(?:\s*,\s*0x[0-9a-f]{2}){5}"
)
USB_MODEM_RE = re.compile(r"/dev/cu\.usbmodem(?P<suffix>[A-Za-z0-9._-]+)")
DEVICE_FINGERPRINT_RE = re.compile(
    r"(?i)\b(?:ea-p4|exoanchor)-(?P<suffix>[0-9a-f]{6})\b"
)
RFC1918_RE = re.compile(
    r"(?<![0-9.])(?:"
    r"10(?:\.[0-9]{1,3}){3}|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])(?:\.[0-9]{1,3}){2}|"
    r"192\.168(?:\.[0-9]{1,3}){2}"
    r")(?![0-9.])"
)
LOCAL_PATH_RE = re.compile(r"(?:file://)?/Users/[A-Za-z0-9._-]+/")
PRIVATE_WORKSPACE_RE = re.compile(r"\bcodexws/(?:private|dev-notes)/")
PRIVATE_KEY_RE = re.compile(
    "-----BEGIN "
    + r"(?:RSA |EC |OPENSSH |DSA )?"
    + r"PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{32,}"
)
TOKEN_RES = (
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9_]{20,}\b"),
    re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
)
SDKCONFIG_AUTH_PASSWORD_RE = re.compile(
    r'(?m)^CONFIG_SI_AUTH_PASSWORD="[^"\r\n]+"\s*$'
)
KCONFIG_AUTH_PASSWORD_BLOCK_RE = re.compile(
    r'(?ms)^\s*config SI_AUTH_PASSWORD\b'
    r'(?P<body>.*?)(?=^\s*(?:config|menu|endmenu)\b|\Z)'
)
KCONFIG_NONEMPTY_DEFAULT_RE = re.compile(
    r'(?m)^\s*default\s+"[^"\r\n]+"\s*$'
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    category: str


def repository_files(repo: Path) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "ls-files",
            "-co",
            "--exclude-standard",
            "-z",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    return [
        Path(item.decode("utf-8"))
        for item in result.stdout.split(b"\0")
        if item
    ]


def should_skip(path: Path) -> bool:
    if path == SELF_PATH:
        return True
    for part in path.parts[:-1]:
        if part in {".git", "components", "managed_components", "__pycache__"}:
            return True
        if part == "build" or part.startswith("build-"):
            return True
    return False


def read_text(path: Path) -> str | None:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) > MAX_TEXT_BYTES or b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def add_matches(
    findings: list[Finding],
    path: Path,
    text: str,
    category: str,
    expression: re.Pattern[str],
) -> None:
    for match in expression.finditer(text):
        findings.append(Finding(path, line_number(text, match.start()), category))


def inspect(path: Path, text: str) -> list[Finding]:
    findings: list[Finding] = []
    add_matches(findings, path, text, "local-user-path", LOCAL_PATH_RE)
    if path.name not in {
        "check-documentation.mjs",
        "check-repository-hygiene.sh",
    }:
        add_matches(
            findings,
            path,
            text,
            "private-workspace-reference",
            PRIVATE_WORKSPACE_RE,
        )
    add_matches(findings, path, text, "private-key-material", PRIVATE_KEY_RE)
    for expression in TOKEN_RES:
        add_matches(findings, path, text, "credential-token", expression)

    add_matches(
        findings,
        path,
        text,
        "embedded-default-password",
        SDKCONFIG_AUTH_PASSWORD_RE,
    )
    if path.name == "Kconfig.projbuild":
        auth_block = KCONFIG_AUTH_PASSWORD_BLOCK_RE.search(text)
        if auth_block:
            default_match = KCONFIG_NONEMPTY_DEFAULT_RE.search(
                auth_block.group("body")
            )
            if default_match:
                findings.append(
                    Finding(
                        path,
                        line_number(text, auth_block.start("body") + default_match.start()),
                        "embedded-default-password",
                    )
                )

    for match in USB_MODEM_RE.finditer(text):
        if match.group("suffix") not in SAFE_USB_SUFFIXES:
            findings.append(
                Finding(path, line_number(text, match.start()), "physical-usb-serial")
            )

    for match in MAC_RE.finditer(text):
        first_octet = int(match.group(0)[:2], 16)
        if first_octet & 0x02 == 0:
            findings.append(
                Finding(path, line_number(text, match.start()), "global-hardware-mac")
            )

    for match in MAC_BYTES_RE.finditer(text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.end())
        if line_end < 0:
            line_end = len(text)
        source_line = text[line_start:line_end]
        if re.search(r"(?i)\bmac\b", source_line) is None:
            continue
        first_octet = int(match.group("first"), 16)
        if first_octet & 0x02 == 0:
            findings.append(
                Finding(path, line_number(text, match.start()), "global-hardware-mac-bytes")
            )

    for match in DEVICE_FINGERPRINT_RE.finditer(text):
        if match.group("suffix").lower() not in SAFE_DEVICE_SUFFIXES:
            findings.append(
                Finding(path, line_number(text, match.start()), "derived-device-fingerprint")
            )

    is_public_narrative = path.suffix.lower() == ".md" or "demo" in path.parts
    is_public_fixture = "tests" in path.parts
    if is_public_narrative or is_public_fixture:
        add_matches(findings, path, text, "private-network-address", RFC1918_RE)
    return findings


def main() -> int:
    repo = (
        Path(sys.argv[1]).resolve()
        if len(sys.argv) > 1
        else Path(__file__).resolve().parents[1]
    )
    findings: list[Finding] = []
    for relative_path in repository_files(repo):
        if should_skip(relative_path):
            continue
        absolute_path = repo / relative_path
        if not absolute_path.is_file():
            continue
        text = read_text(absolute_path)
        if text is None:
            continue
        findings.extend(inspect(relative_path, text))

    unique = sorted(
        set(findings),
        key=lambda item: (item.path.as_posix(), item.line, item.category),
    )
    if unique:
        print("public privacy check failed; matched values are intentionally redacted:")
        for finding in unique:
            print(f"{finding.path}:{finding.line}: {finding.category}")
        return 1
    print("public privacy check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
