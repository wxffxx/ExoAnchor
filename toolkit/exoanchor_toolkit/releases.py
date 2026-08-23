from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterator

from .errors import ToolkitError
from .firmware import MANIFEST_NAME, FirmwarePackage, load_firmware_package


DEFAULT_GITHUB_REPOSITORY = "wxffxx/ExoAnchor"
GITHUB_API_VERSION = "2022-11-28"
MAX_RELEASE_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_EXTRACTED_BYTES = 256 * 1024 * 1024
MAX_ARCHIVE_FILES = 64
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
ASSET_RE = re.compile(
    r"^exoanchor-firmware-[A-Za-z0-9][A-Za-z0-9._-]*\.zip$"
)
DIGEST_RE = re.compile(r"^sha256:([0-9a-fA-F]{64})$")


@dataclass(frozen=True)
class ReleaseAsset:
    asset_id: int
    name: str
    download_url: str
    size: int
    digest: str | None

    def to_dict(self) -> dict[str, object]:
        return {
            "id": self.asset_id,
            "name": self.name,
            "download_url": self.download_url,
            "size": self.size,
            "digest": self.digest,
        }


@dataclass(frozen=True)
class FirmwareRelease:
    release_id: int
    tag: str
    name: str
    published_at: str
    prerelease: bool
    html_url: str
    asset: ReleaseAsset

    @property
    def expected_version(self) -> str:
        value = self.tag
        if value.startswith("firmware-v"):
            return value[len("firmware-v") :]
        if value.startswith("v"):
            return value[1:]
        return value

    def to_dict(self) -> dict[str, object]:
        return {
            "id": self.release_id,
            "tag": self.tag,
            "name": self.name,
            "published_at": self.published_at,
            "prerelease": self.prerelease,
            "html_url": self.html_url,
            "asset": self.asset.to_dict(),
        }


@dataclass(frozen=True)
class DownloadedFirmware:
    repository: str
    release: FirmwareRelease
    package: FirmwarePackage
    directory: Path
    archive_sha256: str
    cached: bool

    def to_dict(self) -> dict[str, object]:
        return {
            "repository": self.repository,
            "release": self.release.to_dict(),
            "package": self.package.to_dict(),
            "directory": str(self.directory),
            "archive_sha256": self.archive_sha256,
            "cached": self.cached,
        }


def validate_repository(repository: str) -> str:
    value = repository.strip()
    if not REPOSITORY_RE.fullmatch(value):
        raise ToolkitError("GitHub repository must use owner/name syntax")
    return value


def default_cache_dir() -> Path:
    if sys.platform == "darwin":
        root = Path.home() / "Library" / "Caches"
    elif os.name == "nt":
        root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    else:
        root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return root / "exoanchor-toolkit" / "firmware"


def _headers(token: str | None = None) -> dict[str, str]:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": GITHUB_API_VERSION,
        "User-Agent": "ExoAnchor-Toolkit",
    }
    value = token or os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if value:
        headers["Authorization"] = "Bearer " + value
    return headers


def _read_json_response(
    request: urllib.request.Request,
    *,
    opener: Callable[..., object],
) -> object:
    try:
        with opener(request, timeout=20) as response:
            content = response.read(MAX_RELEASE_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as exc:
        detail = f"GitHub returned HTTP {exc.code}"
        if exc.code == 403:
            detail += "; API rate limit may be exhausted"
        raise ToolkitError(detail) from exc
    except (urllib.error.URLError, OSError) as exc:
        raise ToolkitError(f"cannot query GitHub releases: {exc}") from exc
    if len(content) > MAX_RELEASE_RESPONSE_BYTES:
        raise ToolkitError("GitHub release response is unexpectedly large")
    try:
        return json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ToolkitError("GitHub returned invalid release metadata") from exc


def _asset_from_json(value: object) -> ReleaseAsset | None:
    if not isinstance(value, dict):
        return None
    name = value.get("name")
    url = value.get("browser_download_url")
    state_value = value.get("state")
    if (
        not isinstance(name, str)
        or not ASSET_RE.fullmatch(name)
        or not isinstance(url, str)
        or not url.startswith("https://github.com/")
        or state_value != "uploaded"
    ):
        return None
    try:
        asset_id = int(value.get("id"))
        size = int(value.get("size"))
    except (TypeError, ValueError):
        return None
    if asset_id <= 0 or not 0 < size <= MAX_ARCHIVE_BYTES:
        return None
    digest_value = value.get("digest")
    digest = digest_value if isinstance(digest_value, str) else None
    if digest is not None and not DIGEST_RE.fullmatch(digest):
        return None
    return ReleaseAsset(
        asset_id=asset_id,
        name=name,
        download_url=url,
        size=size,
        digest=digest.lower() if digest else None,
    )


def _release_from_json(
    value: object,
    *,
    include_prerelease: bool,
) -> FirmwareRelease | None:
    if not isinstance(value, dict) or value.get("draft") is True:
        return None
    prerelease = value.get("prerelease") is True
    if prerelease and not include_prerelease:
        return None
    assets = [
        asset
        for item in value.get("assets", [])
        if (asset := _asset_from_json(item)) is not None
    ] if isinstance(value.get("assets"), list) else []
    if len(assets) != 1:
        return None
    try:
        release_id = int(value.get("id"))
    except (TypeError, ValueError):
        return None
    tag = value.get("tag_name")
    name = value.get("name")
    published_at = value.get("published_at")
    html_url = value.get("html_url")
    if (
        release_id <= 0
        or not isinstance(tag, str)
        or not tag.strip()
        or not isinstance(name, str)
        or not isinstance(published_at, str)
        or not isinstance(html_url, str)
        or not html_url.startswith("https://github.com/")
    ):
        return None
    return FirmwareRelease(
        release_id=release_id,
        tag=tag.strip(),
        name=name.strip() or tag.strip(),
        published_at=published_at,
        prerelease=prerelease,
        html_url=html_url,
        asset=assets[0],
    )


def firmware_releases(
    repository: str = DEFAULT_GITHUB_REPOSITORY,
    *,
    include_prerelease: bool = True,
    token: str | None = None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> list[FirmwareRelease]:
    repository = validate_repository(repository)
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repository}/releases?per_page=30",
        headers=_headers(token),
    )
    payload = _read_json_response(request, opener=opener)
    if not isinstance(payload, list):
        raise ToolkitError("GitHub release response must be a list")
    releases = [
        release
        for item in payload
        if (
            release := _release_from_json(
                item,
                include_prerelease=include_prerelease,
            )
        )
        is not None
    ]
    if not releases:
        qualifier = "stable " if not include_prerelease else ""
        raise ToolkitError(
            f"no {qualifier}ExoAnchor firmware package release found in {repository}"
        )
    releases.sort(key=lambda item: item.published_at, reverse=True)
    return releases


def latest_firmware_release(
    repository: str = DEFAULT_GITHUB_REPOSITORY,
    *,
    include_prerelease: bool = True,
    token: str | None = None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> FirmwareRelease:
    return firmware_releases(
        repository,
        include_prerelease=include_prerelease,
        token=token,
        opener=opener,
    )[0]


def firmware_release_by_tag(
    repository: str,
    tag: str,
    *,
    include_prerelease: bool = True,
    token: str | None = None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> FirmwareRelease:
    requested = tag.strip()
    if not requested:
        raise ToolkitError("firmware release tag is required")
    releases = firmware_releases(
        repository,
        include_prerelease=include_prerelease,
        token=token,
        opener=opener,
    )
    for release in releases:
        if release.tag == requested:
            return release
    raise ToolkitError(
        f"firmware release {requested} is not available in "
        f"{validate_repository(repository)}"
    )


def _safe_target_name(release: FirmwareRelease) -> str:
    stem = Path(release.asset.name).stem
    identity = (
        DIGEST_RE.fullmatch(release.asset.digest).group(1)[:12]
        if release.asset.digest
        else str(release.asset.asset_id)
    )
    return f"{stem}-{identity}"


def _load_cached(
    target: Path,
    release: FirmwareRelease,
) -> FirmwarePackage | None:
    if not target.exists():
        return None
    if target.is_symlink():
        raise ToolkitError(f"cached firmware target cannot be a symlink: {target}")
    manifests = list(target.rglob(MANIFEST_NAME)) if target.is_dir() else []
    if len(manifests) != 1:
        raise ToolkitError(f"cached firmware directory is invalid: {target}")
    package = load_firmware_package(manifests[0])
    if package.version != release.expected_version:
        raise ToolkitError(
            f"cached firmware version {package.version} does not match "
            f"release {release.tag}"
        )
    return package


def _download_archive(
    asset: ReleaseAsset,
    destination: Path,
    *,
    token: str | None,
    opener: Callable[..., object],
) -> str:
    request = urllib.request.Request(asset.download_url, headers=_headers(token))
    digest = hashlib.sha256()
    received = 0
    try:
        with opener(request, timeout=60) as response, destination.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                received += len(chunk)
                if received > MAX_ARCHIVE_BYTES or received > asset.size:
                    raise ToolkitError("firmware release asset exceeds its declared size")
                digest.update(chunk)
                output.write(chunk)
    except ToolkitError:
        raise
    except urllib.error.HTTPError as exc:
        raise ToolkitError(
            f"firmware asset download returned HTTP {exc.code}"
        ) from exc
    except (urllib.error.URLError, OSError) as exc:
        raise ToolkitError(f"cannot download firmware asset: {exc}") from exc
    if received != asset.size:
        raise ToolkitError(
            f"firmware asset size mismatch: expected {asset.size}, received {received}"
        )
    actual = digest.hexdigest()
    if asset.digest:
        expected = DIGEST_RE.fullmatch(asset.digest).group(1).lower()
        if actual != expected:
            raise ToolkitError(
                f"firmware archive SHA-256 mismatch: expected {expected}, found {actual}"
            )
    return actual


def _extract_archive(archive: Path, destination: Path) -> Path:
    try:
        with zipfile.ZipFile(archive) as package_zip:
            entries = package_zip.infolist()
            if not entries or len(entries) > MAX_ARCHIVE_FILES:
                raise ToolkitError("firmware archive has an invalid file count")
            total = 0
            seen_files: set[str] = set()
            for entry in entries:
                relative = PurePosixPath(entry.filename)
                mode = entry.external_attr >> 16
                if (
                    relative.is_absolute()
                    or ".." in relative.parts
                    or "\\" in entry.filename
                    or stat.S_ISLNK(mode)
                ):
                    raise ToolkitError(
                        f"unsafe path in firmware archive: {entry.filename}"
                    )
                total += entry.file_size
                if total > MAX_EXTRACTED_BYTES:
                    raise ToolkitError("firmware archive expands beyond the safety limit")
                if entry.is_dir():
                    continue
                normalized = relative.as_posix()
                if normalized in seen_files:
                    raise ToolkitError(
                        f"duplicate path in firmware archive: {entry.filename}"
                    )
                seen_files.add(normalized)
                output = destination.joinpath(*relative.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                with package_zip.open(entry) as source, output.open("wb") as target:
                    shutil.copyfileobj(source, target, length=1024 * 1024)
    except ToolkitError:
        raise
    except (OSError, zipfile.BadZipFile) as exc:
        raise ToolkitError(f"cannot extract firmware archive: {exc}") from exc
    manifests = list(destination.rglob(MANIFEST_NAME))
    if len(manifests) != 1:
        raise ToolkitError(
            "firmware archive must contain exactly one exoanchor-firmware.json"
        )
    return manifests[0]


@contextmanager
def open_firmware_package(
    package: str | Path,
) -> Iterator[FirmwarePackage]:
    path = Path(package).expanduser().resolve()
    if path.suffix.lower() != ".zip":
        yield load_firmware_package(path)
        return
    with tempfile.TemporaryDirectory(prefix=".exoanchor-package-") as temp:
        destination = Path(temp)
        manifest = _extract_archive(path, destination)
        yield load_firmware_package(manifest)


def download_firmware_release(
    repository: str = DEFAULT_GITHUB_REPOSITORY,
    *,
    tag: str | None = None,
    output_dir: str | Path | None = None,
    include_prerelease: bool = True,
    token: str | None = None,
    api_opener: Callable[..., object] = urllib.request.urlopen,
    download_opener: Callable[..., object] = urllib.request.urlopen,
) -> DownloadedFirmware:
    repository = validate_repository(repository)
    release = (
        firmware_release_by_tag(
            repository,
            tag,
            include_prerelease=include_prerelease,
            token=token,
            opener=api_opener,
        )
        if tag is not None
        else latest_firmware_release(
            repository,
            include_prerelease=include_prerelease,
            token=token,
            opener=api_opener,
        )
    )
    parent = (
        Path(output_dir).expanduser().resolve()
        if output_dir is not None
        else default_cache_dir().resolve()
    )
    parent.mkdir(parents=True, exist_ok=True)
    target = parent / _safe_target_name(release)
    cached = _load_cached(target, release)
    if cached is not None:
        digest = (
            DIGEST_RE.fullmatch(release.asset.digest).group(1).lower()
            if release.asset.digest
            else ""
        )
        return DownloadedFirmware(
            repository=repository,
            release=release,
            package=cached,
            directory=target,
            archive_sha256=digest,
            cached=True,
        )

    try:
        with tempfile.TemporaryDirectory(prefix=".exoanchor-", dir=parent) as temp:
            temp_root = Path(temp)
            archive = temp_root / release.asset.name
            actual_digest = _download_archive(
                release.asset,
                archive,
                token=token,
                opener=download_opener,
            )
            extracted = temp_root / "package"
            extracted.mkdir()
            manifest = _extract_archive(archive, extracted)
            package = load_firmware_package(manifest)
            if package.version != release.expected_version:
                raise ToolkitError(
                    f"firmware package version {package.version} does not match "
                    f"release {release.tag}"
                )
            manifest_relative = manifest.relative_to(extracted)
            extracted.rename(target)
            package = load_firmware_package(target / manifest_relative)
    except FileExistsError as exc:
        raise ToolkitError(f"firmware cache target already exists: {target}") from exc
    return DownloadedFirmware(
        repository=repository,
        release=release,
        package=package,
        directory=target,
        archive_sha256=actual_digest,
        cached=False,
    )


def download_latest_firmware(
    repository: str = DEFAULT_GITHUB_REPOSITORY,
    *,
    output_dir: str | Path | None = None,
    include_prerelease: bool = True,
    token: str | None = None,
    api_opener: Callable[..., object] = urllib.request.urlopen,
    download_opener: Callable[..., object] = urllib.request.urlopen,
) -> DownloadedFirmware:
    return download_firmware_release(
        repository,
        output_dir=output_dir,
        include_prerelease=include_prerelease,
        token=token,
        api_opener=api_opener,
        download_opener=download_opener,
    )
