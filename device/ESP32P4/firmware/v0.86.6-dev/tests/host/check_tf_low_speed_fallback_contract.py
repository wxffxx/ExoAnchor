#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
board_config = (ROOT / "main/config/board_config.h").read_text(encoding="utf-8")
storage_header = (ROOT / "main/drivers/storage_manager.h").read_text(encoding="utf-8")
storage = (ROOT / "main/drivers/storage_manager.c").read_text(encoding="utf-8")
observation = (ROOT / "main/application/device_observation_service.c").read_text(encoding="utf-8")
device_http = (ROOT / "main/services/device_http.c").read_text(encoding="utf-8")
diag = (ROOT / "main/services/diag_cli.c").read_text(encoding="utf-8")

fallback_marker = "config SI_TF_SDMMC_LOW_SPEED_FALLBACK"
frequency_marker = "config SI_TF_SDMMC_LOW_SPEED_FREQ_KHZ"
fallback_parts = kconfig.split(fallback_marker, 1)
if len(fallback_parts) != 2:
    raise AssertionError(f"Kconfig TF fallback: missing {fallback_marker!r}")
fallback_parts = fallback_parts[1].split(frequency_marker, 1)
if len(fallback_parts) != 2:
    raise AssertionError(f"Kconfig TF fallback: missing {frequency_marker!r}")
fallback_block = fallback_parts[0]
for fragment in (
    'bool "Retry TF card in low-speed 1-bit mode"',
    "default n",
):
    require(fallback_block, fragment, "Kconfig TF fallback")

for fragment in (
    frequency_marker,
    "default 10000",
):
    require(kconfig, fragment, "Kconfig TF fallback")

require(board_config, "SI_CFG_TF_SDMMC_LOW_SPEED_FALLBACK 1", "board fallback feature macro")

for fragment in (
    "tf_transport_error_allows_low_speed",
    "ESP_ERR_INVALID_RESPONSE",
    "mount_tf_sdmmc(&mount_config, SI_CFG_TF_SDMMC_BUS_WIDTH",
    "mount_tf_sdmmc(&mount_config, 1,",
    'active_mode = "sdmmc-low-speed"',
    "degraded_mode = true",
):
    require(storage, fragment, "storage fallback implementation")

if storage.index("mount_tf_sdmmc(&mount_config, SI_CFG_TF_SDMMC_BUS_WIDTH") >= storage.index("mount_tf_sdmmc(&mount_config, 1,"):
    raise AssertionError("low-speed mount must follow the standard-width attempt")

for fragment in ("degraded_mode", "bus_frequency_khz", "bus_width", "bus_mode", "fallback_reason"):
    require(storage_header, fragment, "TF status contract")
    require(observation, fragment, "TF observation propagation")
    require(device_http, f'"{fragment}"', "TF HTTP status")

require(diag, 'tf mode=%s degraded=%d width=%u freq=%', "TF diagnostic status")

print("TF low-speed fallback contract: PASS")
