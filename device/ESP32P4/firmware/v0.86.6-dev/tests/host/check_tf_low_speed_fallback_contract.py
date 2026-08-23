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
typew = (ROOT / "configs/boards/sdkconfig.defaults.exoanchor-production-typew-local").read_text(encoding="utf-8")
build_script = (ROOT / "tools/build-firmware.sh").read_text(encoding="utf-8")

for fragment in (
    "config SI_TF_SDMMC_LOW_SPEED_FALLBACK",
    "default y if SI_BOARD_EXOANCHOR_PRODUCTION_TYPEW",
    "config SI_TF_SDMMC_LOW_SPEED_FREQ_KHZ",
    "default 10000",
):
    require(kconfig, fragment, "Kconfig TF fallback")

require(board_config, "SI_CFG_TF_SDMMC_LOW_SPEED_FALLBACK 1", "board fallback feature macro")
require(typew, "CONFIG_SI_TF_SDMMC_LOW_SPEED_FALLBACK=y", "TypeW fallback default")
require(typew, "CONFIG_SI_TF_SDMMC_LOW_SPEED_FREQ_KHZ=10000", "TypeW fallback rate")
require(typew, "CONFIG_SI_TF_SDMMC_D2_GPIO=42", "TypeW crossed DAT2 route")
require(typew, "CONFIG_SI_TF_SDMMC_D3_GPIO=41", "TypeW crossed DAT3 route")
require(typew, "TypeW W0.1 profile", "W0.1 hardware revision boundary")
require(typew, "Formal W repairs DAT2/DAT3", "formal W canonical mapping boundary")

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
require(build_script, "exoanchor-production-typew-local (legacy ID; local-only TypeW W0.1 profile)", "TypeW deterministic build support")

print("TF low-speed fallback contract: PASS")
