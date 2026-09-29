#!/usr/bin/env python3
"""Guard the memory, Flash-map, and HTTP responsiveness contracts."""

from __future__ import annotations

import csv
from pathlib import Path
import re
import sys


PROJECT_DIR = Path(__file__).resolve().parents[2]
FLASH_BYTES = 16 * 1024 * 1024


def parse_size(value: str) -> int:
    value = value.strip().lower()
    scale = 1
    if value.endswith("k"):
        scale = 1024
        value = value[:-1]
    elif value.endswith("m"):
        scale = 1024 * 1024
        value = value[:-1]
    return int(value, 0) * scale


def read_partitions() -> list[dict[str, int | str]]:
    rows = []
    source = PROJECT_DIR / "partitions.csv"
    for raw in source.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = next(csv.reader([line], skipinitialspace=True))
        if len(fields) < 5:
            raise ValueError(f"invalid partition row: {raw}")
        rows.append(
            {
                "name": fields[0].strip(),
                "type": fields[1].strip(),
                "subtype": fields[2].strip(),
                "offset": parse_size(fields[3]),
                "size": parse_size(fields[4]),
            }
        )
    return rows


def require_marker(
    failures: list[str], relative_path: str, marker: str, description: str
) -> str:
    text = (PROJECT_DIR / relative_path).read_text(encoding="utf-8")
    if marker not in text:
        failures.append(description)
    return text


def require_config_line(
    failures: list[str], text: str, line: str, description: str
) -> None:
    if not re.search(rf"^{re.escape(line)}$", text, re.MULTILINE):
        failures.append(description)


def main() -> int:
    failures: list[str] = []

    defaults = (PROJECT_DIR / "sdkconfig.defaults").read_text(encoding="utf-8")
    expected_defaults = {
        "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": 4096,
        "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": 150000,
    }
    for key, expected in expected_defaults.items():
        match = re.search(rf"^{re.escape(key)}=(\d+)$", defaults, re.MULTILINE)
        if not match or int(match.group(1)) != expected:
            failures.append(f"{key} must remain {expected}")
    for marker, description in (
        (
            "CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_SIZE=y",
            "bootloader must remain size-optimized",
        ),
        (
            "CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y",
            "bootloader log level must preserve partition headroom",
        ),
    ):
        if marker not in defaults:
            failures.append(description)

    for line, description in (
        (
            "CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS=3",
            "PSRAM UVC capture must retain the official triple-buffer ring",
        ),
        (
            "CONFIG_SI_VIDEO_UVC_URBS=4",
            "UVC capture must retain the official four-URB reference ring",
        ),
        (
            "CONFIG_SI_VIDEO_UVC_URB_SIZE=10240",
            "UVC capture URBs must remain 10 KiB",
        ),
        (
            "# CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM is not set",
            "ESP32-P4 DWC DMA buffers must remain in protected internal memory",
        ),
        (
            "# CONFIG_UVC_CHECK_PAYLOAD_HEADER_EOH is not set",
            "MS2109 compatibility requires the optional UVC EOH-bit check disabled",
        ),
    ):
        require_config_line(failures, defaults, line, description)
    if re.search(
        r"^CONFIG_UVC_CHECK_PAYLOAD_HEADER_EOH=y$", defaults, re.MULTILINE
    ):
        failures.append("the optional UVC EOH-bit check must not be re-enabled")
    if re.search(
        r"^CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y$", defaults, re.MULTILINE
    ):
        failures.append("the base profile must not place DWC DMA buffers in PSRAM")

    h264_defaults = (
        PROJECT_DIR / "configs/profiles/sdkconfig.defaults.dev-h264"
    ).read_text(encoding="utf-8")
    app_config = (
        PROJECT_DIR / "main/config/app_config.h"
    ).read_text(encoding="utf-8")
    for line, description in (
        (
            "# CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM is not set",
            "the H.264 development profile must keep DWC DMA in internal memory",
        ),
        (
            "CONFIG_SI_VIDEO_UVC_FRAME_BUFFERS=3",
            "the H.264 development profile must keep triple UVC buffering",
        ),
    ):
        require_config_line(failures, h264_defaults, line, description)
    if re.search(
        r"^CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y$",
        h264_defaults,
        re.MULTILINE,
    ):
        failures.append(
            "the H.264 development profile must not place DWC DMA buffers in PSRAM"
        )
    for obsolete in (
        "CONFIG_SI_VIDEO_H264_UVC_URBS",
        "CONFIG_SI_VIDEO_H264_UVC_URB_SIZE",
    ):
        if obsolete in h264_defaults or obsolete in app_config:
            failures.append(
                f"optional codecs must not override the Stable UVC topology: {obsolete}"
            )
    for marker, description in (
        (
            "#define SI_CFG_VIDEO_URBS CONFIG_SI_VIDEO_UVC_URBS",
            "every output codec must inherit the Stable four-URB UVC topology",
        ),
        (
            "#define SI_CFG_VIDEO_URB_SIZE CONFIG_SI_VIDEO_UVC_URB_SIZE",
            "every output codec must inherit the Stable 10 KiB UVC URB size",
        ),
    ):
        if marker not in app_config:
            failures.append(description)

    try:
        partitions = read_partitions()
    except (OSError, ValueError) as exc:
        failures.append(f"cannot parse partitions.csv: {exc}")
        partitions = []

    previous_end = 0
    allocated = 0
    for part in sorted(partitions, key=lambda item: int(item["offset"])):
        offset = int(part["offset"])
        size = int(part["size"])
        end = offset + size
        if offset < previous_end:
            failures.append(f"partition {part['name']} overlaps the previous partition")
        if end > FLASH_BYTES:
            failures.append(f"partition {part['name']} exceeds the 16 MiB Flash")
        previous_end = max(previous_end, end)
        allocated += size

    nvs = next((part for part in partitions if part["name"] == "nvs"), None)
    if not nvs or int(nvs["size"]) < 0x6000:
        failures.append("NVS partition must be at least 24 KiB")

    apps = [part for part in partitions if part["type"] == "app"]
    if len(apps) != 3:
        failures.append("factory plus two OTA app slots are required")
    elif any(int(part["size"]) < 0x400000 for part in apps):
        failures.append("every app slot must remain at least 4 MiB")
    elif len({int(part["size"]) for part in apps}) != 1:
        failures.append("factory and OTA app slots must have equal capacity")

    if allocated > FLASH_BYTES:
        failures.append("partition allocation exceeds physical Flash")
    elif FLASH_BYTES - allocated < FLASH_BYTES // 5:
        failures.append("keep at least 20% of Flash outside allocated partitions")

    require_marker(
        failures,
        "main/app/app_main.c",
        "Initialization complete; releasing app_main task stack",
        "app_main must release its one-shot initialization stack",
    )
    web_server = require_marker(
        failures,
        "main/services/web_server.c",
        "cJSON_InitHooks(&hooks)",
        "cJSON must use the PSRAM-first allocator",
    )
    if "config.recv_wait_timeout = 1;" not in web_server:
        failures.append("main HTTP receive timeout must remain bounded to one second")

    for relative_path, marker, description in (
        (
            "main/services/web/async_http_module.inc",
            "xTaskCreateWithCaps(long_http_worker",
            "long HTTP operations must use a PSRAM-backed worker",
        ),
        (
            "main/services/web/base_settings_http_module.inc",
            "xTaskCreateWithCaps(auth_login_worker",
            "password verification must use a PSRAM-backed worker",
        ),
        (
            "main/services/ssh_client.c",
            "SI_SSH_IO_WAIT_SLICE_MS 250U",
            "SSH cancellation polling must remain responsive",
        ),
        (
            "main/services/web/ssh_module.inc",
            "ssh_ws_ctx_cleanup_task",
            "SSH WebSocket cleanup must stay off the main HTTP task",
        ),
        (
            "main/drivers/storage_manager.c",
            "storage_maintenance_task",
            "TF probing and capacity refresh must stay in a background task",
        ),
        (
            "main/services/device_http.c",
            '"partition_map_allocated"',
            "Flash usage must report allocated partitions, not layout high-water",
        ),
        (
            "main/application/device_observation_service.c",
            "nvs_get_stats(NULL, &nvs_stats)",
            "NVS capacity must remain observable",
        ),
    ):
        require_marker(failures, relative_path, marker, description)

    http_api = (PROJECT_DIR / "main/adapters/http_api.c").read_text(
        encoding="utf-8"
    )
    if "request_header_dup" in http_api:
        failures.append("authenticated requests must not heap-duplicate headers")
    if "request_header_copy" not in http_api:
        failures.append("authenticated headers must use bounded caller storage")

    uart_module = (PROJECT_DIR / "main/services/web/uart_module.inc").read_text(
        encoding="utf-8"
    )
    if "xSemaphoreTake(s_uart_ws_lock, portMAX_DELAY)" in uart_module:
        failures.append("UART WebSocket handling must not wait forever on its mutex")

    video_http = (
        PROJECT_DIR / "main/services/web/video_http_module.inc"
    ).read_text(encoding="utf-8")
    stream_start = video_http.find("static esp_err_t stream_handler(")
    stream_end = video_http.find("static int httpd_client_count(")
    stream_handler = video_http[stream_start:stream_end]
    pace_pos = stream_handler.find(
        "if (frame_interval > 0 && next_send_tick != 0"
    )
    gate_pos = stream_handler.find("si_video_begin_mjpeg_send(1200)")
    borrow_pos = stream_handler.find("si_video_borrow_jpeg_if_new(")
    consume_pos = stream_handler.find("last_frame_id = frame_id;")
    payload_pos = stream_handler.find("(const char *)jpeg_view.data, jpeg_len")
    release_pos = stream_handler.find("si_video_release_jpeg(&jpeg_view);",
                                      payload_pos)
    frame_id_pos = stream_handler.find("const uint32_t frame_id = jpeg_view.frame_id;")
    gate_release_pos = stream_handler.find("si_video_end_mjpeg_send();",
                                           frame_id_pos)
    if not (
        0 <= pace_pos < gate_pos < borrow_pos < frame_id_pos < gate_release_pos < payload_pos
        < release_pos < consume_pos
    ):
        failures.append(
            "MJPEG must pace, borrow an immutable frame, release the mode gate before network I/O, then release the view"
        )

    video_input = (
        PROJECT_DIR / "main/drivers/video_input.c"
    ).read_text(encoding="utf-8")
    reserve_start = video_input.find(
        "static bool try_reserve_uvc_frame(void)")
    reserve_end = video_input.find("static bool uvc_frame_cb(", reserve_start)
    reserve = video_input[reserve_start:reserve_end]
    if not (
        reserve_start >= 0
        and "SI_UVC_RETENTION_CAP_BIT" in reserve
        and "SI_UVC_RETENTION_COUNT_MASK" in reserve
        and "SI_CFG_VIDEO_FRAME_BUFFERS - 1U" in reserve
        and reserve.count(
            "__atomic_compare_exchange_n(&s_uvc_retention_state") >= 2
        and "try_enable_uvc_retention_cap" in reserve
        and "disable_uvc_retention_cap" in reserve
        and "__atomic_fetch_and(&s_uvc_retention_state" in reserve
        and "xSemaphoreTake(" not in reserve
        and "uvc_host_frame_return(" not in reserve
    ):
        failures.append(
            "H.264 UVC callback admission must apply a transition-safe N-1 "
            "packed-atomic retention cap"
        )

    release_state_start = video_input.find(
        "static void release_uvc_frame_retention(void)")
    release_state_end = video_input.find(
        "static void return_retained_uvc_frame(", release_state_start)
    release_state = video_input[release_state_start:release_state_end]
    if not (
        release_state_start >= 0
        and "assert(count > 0U);" in release_state
        and "state & SI_UVC_RETENTION_CAP_BIT" in release_state
        and "__atomic_compare_exchange_n(&s_uvc_retention_state" in release_state
    ):
        failures.append(
            "UVC retention release must assert single ownership and decrement "
            "only the packed count bits"
        )

    callback_start = video_input.find("static bool uvc_frame_cb(")
    callback_end = video_input.find("static void uvc_ingest_task(",
                                    callback_start)
    callback = video_input[callback_start:callback_end]
    accept_pos = callback.find("s_uvc_accept_frames")
    reserve_pos = callback.find("try_reserve_uvc_frame()", accept_pos)
    pressure_pos = callback.find(
        "__atomic_add_fetch(&s_h264_pressure_coalesces", reserve_pos)
    pressure_return_pos = callback.find("return true;", pressure_pos)
    queue_pos = callback.find("xQueueSend(s_uvc_frame_queue, &item, 0)",
                              pressure_return_pos)
    rollback_pos = callback.find("release_uvc_frame_retention()", queue_pos)
    drop_pos = callback.find("__atomic_add_fetch(&s_uvc_queue_drops",
                             rollback_pos)
    return_on_full_pos = callback.find("return true;", drop_pos)
    retained_pos = callback.find("return false;", return_on_full_pos)
    if not (
        0 <= accept_pos < reserve_pos < pressure_pos
        < pressure_return_pos < queue_pos < rollback_pos < drop_pos
        < return_on_full_pos < retained_pos
    ):
        failures.append(
            "UVC callback must reject invalid generations, reserve one physical "
            "driver buffer during H.264, record intentional pressure separately, "
            "and roll back retention when ingest handoff fails"
        )

    transport_start = video_input.find(
        "esp_err_t si_video_set_h264_transport_active(bool active)")
    transport_end = video_input.find("size_t si_video_jpeg_capacity(void)",
                                     transport_start)
    transport = video_input[transport_start:transport_end]
    enable_start = transport.find("if (active) {")
    cap_enable = transport.find(
        "while (!try_enable_uvc_retention_cap())", enable_start)
    publish_active = transport.find(
        "__atomic_store_n(&s_h264_transport_active, true", cap_enable)
    disable_start = transport.find("} else {", publish_active)
    publish_inactive = transport.find(
        "__atomic_store_n(&s_h264_transport_active, false", disable_start)
    cap_disable = transport.find("disable_uvc_retention_cap()",
                                 publish_inactive)
    if not (
        0 <= enable_start < cap_enable < publish_active
        < disable_start < publish_inactive < cap_disable
    ):
        failures.append(
            "H.264 transition must enable and settle the UVC retention cap "
            "before publishing active, then publish inactive before releasing it"
        )
    for forbidden in (
        "si_mjpeg_validate(",
        "si_video_frame_store_publish(",
        "uvc_host_frame_return(",
        "memcpy(",
        "xSemaphoreTake(",
    ):
        if forbidden in callback:
            failures.append(
                f"UVC callback performs ingest work instead of a bounded queue handoff: {forbidden}"
            )

    ingest_start = video_input.find("static void uvc_ingest_task(")
    ingest_end = video_input.find("static void uvc_stream_event_cb(",
                                  ingest_start)
    ingest = video_input[ingest_start:ingest_end]
    receive_pos = ingest.find("xQueueReceive(s_uvc_frame_queue, &item,")
    consume_pos = ingest.find("const int64_t ingest_started_us", receive_pos)
    validate_pos = ingest.find("si_mjpeg_validate(", consume_pos)
    snapshot_pos = ingest.find("s_mjpeg_snapshot_request_generation",
                               validate_pos)
    snapshot_publish_pos = ingest.find(
        "si_video_frame_store_publish(item.frame->data",
        snapshot_pos)
    route_pos = ingest.find("handoff_h264_uvc_frame(&item, &frame_id)",
                            snapshot_publish_pos)
    publish_pos = ingest.find("si_video_frame_store_publish(", route_pos)
    ownership_pos = ingest.find("if (!h264_owns_frame)", publish_pos)
    frame_return_pos = ingest.find("return_retained_uvc_frame(&item)",
                                   ownership_pos)
    if not (
        0 <= receive_pos < consume_pos < validate_pos < snapshot_pos
        < snapshot_publish_pos < route_pos < publish_pos
        < ownership_pos < frame_return_pos
    ) or "continue;" in ingest[consume_pos:frame_return_pos]:
        failures.append(
            "UVC ingest must validate, split direct H.264 leases from copied "
            "MJPEG publication, and return only frames it still owns"
        )
    retained_return_start = video_input.find(
        "static void return_retained_uvc_frame(")
    retained_return_end = video_input.find(
        "static void uvc_return_task(", retained_return_start)
    retained_return = video_input[retained_return_start:retained_return_end]
    if not (
        retained_return.find("uvc_host_frame_return(") >= 0 and
        retained_return.find("uvc_host_frame_return(") <
        retained_return.find("release_uvc_frame_retention()") and
        retained_return.find("release_uvc_frame_retention()") <
        retained_return.find("xQueueSend(s_uvc_return_queue, &pending, 0)") and
        "__atomic_add_fetch(&s_uvc_return_pending" in retained_return
    ):
        failures.append(
            "UVC lease release must return directly before decrementing retention, with a bounded recovery queue fallback"
        )
    return_worker_start = video_input.find(
        "static void uvc_return_task(", retained_return_end)
    return_worker_end = video_input.find(
        "static bool take_queued_h264_frame_locked(", return_worker_start)
    return_worker = video_input[return_worker_start:return_worker_end]
    if not (
        return_worker.find("uvc_host_frame_return(") >= 0 and
        return_worker.find("uvc_host_frame_return(") <
        return_worker.find("release_uvc_frame_retention()")
    ):
        failures.append(
            "UVC return worker must return the host frame before decrementing end-to-end retention"
        )

    frame_store = (
        PROJECT_DIR / "main/drivers/video_frame_store.c"
    ).read_text(encoding="utf-8")
    for marker, description in (
        (
            "128, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
            "frame store must allocate one maximum-frame arena, not two fixed maximum slots",
        ),
        (
            "const size_t other_index = 1U - target;",
            "frame store must pack two logical slots from opposite ends of one arena",
        ),
        (
            "slot->refs != 0 || slot->writing ||",
            "frame store publication must never select a borrowed or in-flight slot",
        ),
        (
            "if (target == s_store.current)",
            "frame store must hide a current slot before overwriting it with the lock released",
        ),
        (
            "memmove(slot->data, data, len);",
            "frame store must copy UVC data before publishing an immutable view",
        ),
        (
            "slot->refs++;",
            "frame store acquire must retain its immutable slot",
        ),
        (
            "slot->refs--;",
            "frame store release must drop the immutable slot reference",
        ),
        (
            "return ESP_ERR_NOT_SUPPORTED;",
            "packed frame store must explicitly reject the retired full-arena scratch API",
        ),
    ):
        if marker not in frame_store:
            failures.append(description)
    if "capacity * SI_VIDEO_FRAME_STORE_SLOTS" in frame_store:
        failures.append(
            "frame store regressed to two fixed maximum-size payload arenas"
        )
    if "si_video_frame_store_init(s_uvc_frame_capacity)" not in video_input:
        failures.append(
            "immutable MJPEG store must accept the same maximum frame as the UVC arena"
        )

    h264_stream = (
        PROJECT_DIR / "main/services/video_h264_stream.c"
    ).read_text(encoding="utf-8")
    for forbidden in (
        "si_h264_stream_stage_mjpeg_frame",
        "si_h264_stream_release_mjpeg_frame",
        "si_h264_mjpeg_staged_frame_t",
        "s_preallocated_jpeg_input",
    ):
        if forbidden in h264_stream or forbidden in video_http:
            failures.append(
                f"obsolete H.264-owned MJPEG staging API remains: {forbidden}"
            )
    for marker in (
        "si_video_borrow_h264_jpeg_if_new(last_jpeg_frame, &jpeg_view)",
        ".jpeg_view = jpeg_view,",
        "si_video_release_h264_jpeg(&job->jpeg_view)",
        "si_video_release_h264_jpeg(&jpeg_view)",
    ):
        if marker not in h264_stream:
            failures.append(
                f"H.264 must consume and release the direct UVC lease: {marker}"
            )

    decoder_start = h264_stream.find(
        "static esp_err_t h264_jpeg_decoder_process("
    )
    decoder_end = h264_stream.find("typedef struct {", decoder_start)
    decoder_wrapper = h264_stream[decoder_start:decoder_end]
    if "return jpeg_decoder_process(" not in decoder_wrapper:
        failures.append("H.264 must call the official JPEG decoder directly")
    video_mjpeg = (
        PROJECT_DIR / "main/drivers/video_mjpeg.c"
    ).read_text(encoding="utf-8")
    project_cmake = (PROJECT_DIR / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    validator_start = video_mjpeg.find("bool si_mjpeg_validate(")
    validator = video_mjpeg[validator_start:]
    for marker, description in (
        (
            "const uint8_t *data, size_t len,",
            "MJPEG structural validation input must remain immutable",
        ),
        (
            "data[0] != 0xff || data[1] != 0xd8",
            "MJPEG structural validation must require an exact SOI",
        ),
        (
            "data[len - 2] != 0xff || data[len - 1] != 0xd9",
            "MJPEG structural validation must require an exact terminal EOI",
        ),
        (
            "while (pos + 1 < len)",
            "MJPEG structural validation must use a length-bounded header walk",
        ),
        (
            "segment_len < 2 || pos + segment_len > len - 2U",
            "MJPEG header segments must end before the terminal EOI",
        ),
        (
            "if (marker_is_sof(marker))",
            "MJPEG structural validation must locate SOF dimensions",
        ),
        (
            "if (marker == 0xda)",
            "MJPEG structural validation must stop at SOS",
        ),
        (
            "expected_width > 0 && width != expected_width",
            "MJPEG SOF width must match the negotiated capture mode",
        ),
        (
            "expected_height > 0 && height != expected_height",
            "MJPEG SOF height must match the negotiated capture mode",
        ),
        (
            "Entropy is intentionally opaque on the capture hot path.",
            "MJPEG validator must document downstream decoder-owned entropy",
        ),
        (
            "downstream JPEG decoder",
            "MJPEG validator must assign entropy validation to the decoder",
        ),
        (
            "Never repair or synthesize JPEG data here.",
            "MJPEG validator must explicitly forbid JPEG repair or synthesis",
        ),
    ):
        if marker not in validator:
            failures.append(description)
    if validator_start < 0 or validator.count("while (") != 2 or "for (" in validator:
        failures.append(
            "MJPEG capture validation may walk only the bounded marker headers before SOS"
        )
    capture_validation_chain = "\n".join(
        (project_cmake, video_input, video_mjpeg, h264_stream)
    )
    for forbidden_scan in (
        "memchr(",
        "isoc_find_jpeg_marker(",
        "isoc_mjpeg_trim_to_eoi(",
        "mjpeg_complete_len(",
    ):
        if forbidden_scan in capture_validation_chain:
            failures.append(
                "capture hot path must not scan or normalize MJPEG entropy: "
                + forbidden_scan
            )
    video_chain = "\n".join(
        (project_cmake, video_input, h264_stream, video_mjpeg)
    )
    for forbidden in (
        "esp_intr_disable(",
        "esp_intr_enable(",
        "REG_WRITE(",
        "REG_SET_BIT(",
        "REG_CLR_BIT(",
        "mjpeg_complete_len(",
        "isoc_mjpeg_trim_to_eoi(",
        "isoc_add_mjpeg_frame_data(",
        "isoc_find_jpeg_marker(",
        "portDISABLE_INTERRUPTS",
        "portENTER_CRITICAL",
    ):
        if forbidden in video_chain:
            failures.append(
                f"video chain must not repair JPEG entropy or mask codec interrupts: {forbidden}"
            )
    for pattern in (
        r"(?:jpeg_view\.data|frame_input|item\.frame->data)\s*\[[^]]+\]\s*=",
        r"mem(?:cpy|move|set)\s*\(\s*(?:jpeg_view\.data|frame_input|item\.frame->data)",
    ):
        if re.search(pattern, video_chain):
            failures.append("validated JPEG input must never be repaired or mutated")

    mjpeg_gate_start = video_input.find(
        "esp_err_t si_video_begin_mjpeg_send(uint32_t timeout_ms)"
    )
    mjpeg_gate_end = video_input.find("void si_video_end_mjpeg_send(void)",
                                      mjpeg_gate_start)
    mjpeg_gate = video_input[mjpeg_gate_start:mjpeg_gate_end]
    h264_gate_start = video_input.find(
        "esp_err_t si_video_set_h264_transport_active(bool active)"
    )
    h264_gate_end = video_input.find("size_t si_video_jpeg_capacity(void)",
                                     h264_gate_start)
    h264_gate = video_input[h264_gate_start:h264_gate_end]
    if (
        "xSemaphoreTake(s_transport_gate," not in mjpeg_gate
        or "s_h264_transport_active" not in mjpeg_gate
        or "xSemaphoreGive(s_transport_gate);" not in mjpeg_gate
    ):
        failures.append("MJPEG output must acquire and validate the shared output gate")
    if (
        "xSemaphoreTake(s_transport_gate," not in h264_gate
        or "__atomic_store_n(&s_h264_transport_active, true" not in h264_gate
        or "__atomic_store_n(&s_h264_transport_active, false" not in h264_gate
        or "xSemaphoreGive(s_transport_gate);" not in h264_gate
    ):
        failures.append("H.264 output must exclusively own and release the shared output gate")

    if (
        "idf_component_get_property(si_uvc_component espressif__usb_host_uvc COMPONENT_LIB)"
        not in project_cmake
    ):
        failures.append("video input must use Espressif's registered usb_host_uvc component")
    for marker in (
        '"c709685f2814697c568620c7c2b7974e3e3167a92308ddd324c46db082de1ccc"',
        'file(SHA256 "${si_uvc_upstream_isoc}" si_uvc_isoc_actual_sha256)',
        "set(si_uvc_loss_upstream [=[",
        "set(si_uvc_loss_fixed [=[",
        "if (loss_frame && !completed_mjpeg_waiting_for_fid)",
        "uvc_stream->single_thread.skip_current_frame = true;",
        'string(REPLACE "${si_uvc_loss_upstream}" "${si_uvc_loss_fixed}"',
        'si_uvc_isoc_loss_patched "${si_uvc_isoc_source}")',
        'string(REPLACE "${si_uvc_sof_upstream}" "${si_uvc_sof_fixed}"',
        'si_uvc_isoc_patched "${si_uvc_isoc_loss_patched}")',
        'set(si_uvc_patched_isoc "${si_uvc_patch_dir}/uvc_isoc.c")',
    ):
        if marker not in project_cmake:
            failures.append(
                f"bounded generated-source MS2109 FID patch missing marker: {marker}"
            )
    for forbidden in (
        "si_uvc_loss_status_",
        "si_uvc_compat_isoc",
        "isoc_add_mjpeg_frame_data",
        "isoc_mjpeg_trim_to_eoi",
        "isoc_find_jpeg_marker",
        "mjpeg_complete_len",
        "Using ExoAnchor UVC MJPEG SOI/EOI compatibility source",
    ):
        if forbidden in project_cmake:
            failures.append(
                f"custom UVC isochronous assembly override remains: {forbidden}"
            )
    if (PROJECT_DIR / "patches/usb_host_uvc-2.5.1/uvc_isoc.c").exists():
        failures.append("custom usb_host_uvc/uvc_isoc.c source must not be shipped")

    web_server = (PROJECT_DIR / "main/services/web_server.c").read_text(
        encoding="utf-8"
    )
    async_http = (
        PROJECT_DIR / "main/services/web/async_http_module.inc"
    ).read_text(encoding="utf-8")
    if 'register_uri(s_server, "/api/stream", HTTP_GET,\n                 stream_async_handler, false);' not in web_server:
        failures.append("browser MJPEG must use the async same-origin route")
    if "DEFINE_LONG_HTTP_WRAPPER(stream_async_handler, stream_handler," not in async_http:
        if "dispatch_http_worker(req, stream_handler, \"video.stream\"," not in async_http:
            failures.append("same-origin MJPEG must run outside the main HTTP task")
    for marker, description in (
        (
            "#define SI_VIDEO_HTTP_MAX_WORKERS 2",
            "MJPEG reconnect overlap needs two dedicated worker slots",
        ),
        (
            "s_video_http_slots",
            "MJPEG must not consume the shared long-operation slots",
        ),
    ):
        if marker not in async_http:
            failures.append(description)
    kvm_page = (PROJECT_DIR / "main/www/kvm.html").read_text(encoding="utf-8")
    agent_page = (PROJECT_DIR / "main/www/agent.html").read_text(encoding="utf-8")
    if ':81/api/stream' in kvm_page or ':81/api/stream' in agent_page:
        failures.append("browser MJPEG must remain on the async same-origin route")
    if 'if(location.protocol==="http:")url.port="81"' not in kvm_page:
        failures.append("browser H.264 must use the dedicated video HTTPD on port 81")
    if ':81/api/ws/video/h264' in agent_page:
        failures.append("Agent page must not open a human H.264 video socket")

    agent_status_snapshot = (
        PROJECT_DIR / "main/services/web/agent_run_status_snapshot_module.inc"
    ).read_text(encoding="utf-8")
    for marker, description in (
        (
            "agent_run_status_snapshot_take(",
            "Agent status must snapshot state before JSON serialization",
        ),
        (
            "xSemaphoreGive(s_agent_run_lock);\n    return snapshot;",
            "Agent status snapshot must release the Run lock before serialization",
        ),
    ):
        if marker not in agent_status_snapshot:
            failures.append(description)

    ssh_header = (
        PROJECT_DIR / "main/services/ssh_client.h"
    ).read_text(encoding="utf-8")
    ssh_client = (
        PROJECT_DIR / "main/services/ssh_client.c"
    ).read_text(encoding="utf-8")
    agent_ssh_tool = (
        PROJECT_DIR / "main/services/web/agent_ssh_tool_module.inc"
    ).read_text(encoding="utf-8")
    if (
        "si_ssh_exec_cancel_cb_t" not in ssh_header
        or "ssh_exec_cancel_requested(config)" not in ssh_client
        or "config->cancel_cb = agent_ssh_cancel_requested;" not in agent_ssh_tool
    ):
        failures.append(
            "Agent SSH execution must cooperatively observe Run cancellation"
        )

    agent_broker_http = (
        PROJECT_DIR / "main/services/web/agent_broker_http_module.inc"
    ).read_text(encoding="utf-8")
    if "vTaskDelay(pdMS_TO_TICKS(50));" in agent_broker_http:
        failures.append(
            "Agent approval waiting must not poll both locks every 50 ms"
        )

    agent_request_builder = (
        PROJECT_DIR / "main/services/web/agent_request_module.inc"
    ).read_text(encoding="utf-8")
    agent_cloud_transport = (
        PROJECT_DIR / "main/services/web/agent_cloud_transport_module.inc"
    ).read_text(encoding="utf-8")
    agent_request = agent_request_builder + "\n" + agent_cloud_transport
    if '#include "agent_cloud_transport_module.inc"' not in agent_request_builder:
        failures.append("Agent request builder must delegate Provider transport")
    for marker, description in (
        (
            "#define AGENT_CLOUD_IO_SLICE_MS 250",
            "HTTPS Provider I/O must remain sliced for responsive cancellation",
        ),
        (
            ".is_async = true",
            "HTTPS Provider requests must use the asynchronous HTTP client",
        ),
        (
            "agent_api_validate_endpoint(url, false) != ESP_OK",
            "Provider transport must reject empty or non-HTTPS endpoints",
        ),
        (
            "!agent_run_ensure_live_source(job_id)",
            "Provider slices must observe Run cancellation and source revoke",
        ),
        (
            "} while (ret == ESP_ERR_HTTP_EAGAIN);",
            "asynchronous Provider requests must advance until completion",
        ),
        (
            "esp_http_client_get_and_clear_last_tls_error(",
            "async Provider retries must surface hard TLS write failures",
        ),
        (
            "si_agent_cloud_async_error_is_fatal(",
            "hard TLS errors must not be retried as readiness slices",
        ),
        (
            "transport_diagnostic_captured = true;",
            "captured async transport diagnostics must survive cleanup",
        ),
        (
            "MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT",
            "Provider diagnostics must report the actual AES DMA heap",
        ),
    ):
        if marker not in agent_request:
            failures.append(description)
    eagain_branch = agent_request.find("if (ret == ESP_ERR_HTTP_EAGAIN) {")
    slice_errno = agent_request.find(
        "esp_http_client_get_errno(client)", eagain_branch
    )
    slice_tls = agent_request.find(
        "esp_http_client_get_and_clear_last_tls_error(", eagain_branch
    )
    fatal_check = agent_request.find(
        "si_agent_cloud_async_error_is_fatal(", eagain_branch
    )
    diagnostic_capture = agent_request.find(
        "transport_diagnostic_captured = true;", eagain_branch
    )
    retry_delay = agent_request.find(
        "vTaskDelay(pdMS_TO_TICKS(20));", eagain_branch
    )
    if not (
        0 <= eagain_branch < slice_errno < slice_tls < fatal_check
        < diagnostic_capture < retry_delay
    ):
        failures.append(
            "async Provider EAGAIN must capture and classify transport errors before retry"
        )
    loop_end = agent_request.find(
        "} while (ret == ESP_ERR_HTTP_EAGAIN);", eagain_branch
    )
    final_diagnostic_guard = agent_request.find(
        "if (!transport_diagnostic_captured) {", loop_end
    )
    final_diagnostic_read = agent_request.find(
        "esp_http_client_get_and_clear_last_tls_error(",
        final_diagnostic_guard,
    )
    if not 0 <= loop_end < final_diagnostic_guard < final_diagnostic_read:
        failures.append(
            "async Provider cleanup must not overwrite captured TLS diagnostics"
        )
    for forbidden in (
        "AGENT_CLOUD_HTTP_FALLBACK_TIMEOUT_MS",
        ".is_async = async_https",
        "AGENT_API_ENDPOINT_DEFAULT",
    ):
        if forbidden in agent_request or forbidden in web_server:
            failures.append(
                f"Provider transport still has a fail-open endpoint fallback: {forbidden}"
            )

    device_http = (
        PROJECT_DIR / "main/services/device_http.c"
    ).read_text(encoding="utf-8")
    if (
        "si_web_ssh_target_available()" not in device_http
        or device_http.count('"supervised", "high", ssh_available, true,') < 8
        or '"observe", "low", ssh_available, true,' not in device_http
    ):
        failures.append(
            "SSH-backed tools must report unavailable until a target and local credential are configured"
        )
    if "esp_http_client_fetch_headers(client)" in agent_request:
        failures.append(
            "Provider requests must not return to the blocking header-fetch path"
        )

    agent_run_task = (
        PROJECT_DIR / "main/services/web/agent_run_task_module.inc"
    ).read_text(encoding="utf-8")
    if (
        "while (xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(200))"
        not in agent_run_task
    ):
        failures.append(
            "Agent completion must not abandon the Run state after one lock timeout"
        )

    agent_tool_dispatch = (
        PROJECT_DIR / "main/services/web/agent_tool_dispatch_module.inc"
    ).read_text(encoding="utf-8")
    if (
        "uint32_t slice_ms = MIN(wait_ms, 50U);" not in agent_tool_dispatch
        or "if (!agent_run_control_point(job_id))" not in agent_tool_dispatch
    ):
        failures.append(
            "Agent HID waits must remain interruptible at cooperative checkpoints"
        )
    agent_run_control = (
        PROJECT_DIR / "main/services/web/agent_run_control_module.inc"
    ).read_text(encoding="utf-8")
    if (
        "Agent control point is waiting for the Run lock"
        not in agent_run_control
        or "vTaskDelay(pdMS_TO_TICKS(10));" not in agent_run_control
    ):
        failures.append(
            "transient Run-lock contention must not be misreported as cancellation"
        )

    agent_device_tools = (
        PROJECT_DIR / "main/services/web/agent_device_tools_module.inc"
    ).read_text(encoding="utf-8")
    if "uint32_t slice_ms = MIN(field_delay_ms, 50U);" not in agent_device_tools:
        failures.append(
            "Agent Console login field waits must remain interruptible"
        )

    agent_request = (
        PROJECT_DIR / "main/services/web/agent_request_module.inc"
    ).read_text(encoding="utf-8")
    for marker, description in (
        (
            '"exoanchor.runtime_capabilities.v1"',
            "Agent prompts must include a structured runtime capability snapshot",
        ),
        (
            '"route_tools"',
            "Agent capability snapshots must expose the active route tool set",
        ),
        (
            '"command_ready"',
            "Agent capability snapshots must distinguish HID command readiness",
        ),
    ):
        if marker not in agent_request:
            failures.append(description)

    ui_core = (PROJECT_DIR / "main/www/assets/ui-core.js").read_text(
        encoding="utf-8"
    )
    for marker, description in (
        (
            "getShared(path, options = {})",
            "Agent status requests must support in-flight GET coalescing",
        ),
        (
            'timeoutError.name = "TimeoutError"',
            "frontend polling must recover from a hung Agent status request",
        ),
    ):
        if marker not in ui_core:
            failures.append(description)

    storage_source = (
        PROJECT_DIR / "main/drivers/storage_manager.c"
    ).read_text(encoding="utf-8")
    getter_marker = "void si_storage_get_tf_status("
    getter_source = (
        storage_source.split(getter_marker, 1)[1]
        if getter_marker in storage_source
        else ""
    )
    if not getter_source or "mount_tf_locked" in getter_source:
        failures.append("TF status getter must only return the cached snapshot")

    if failures:
        for failure in failures:
            print(f"resource contract failure: {failure}", file=sys.stderr)
        return 1
    print(
        "resource contract: PASS "
        f"({len(partitions)} partitions, {allocated} allocated bytes, "
        f"{FLASH_BYTES - allocated} unallocated bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
