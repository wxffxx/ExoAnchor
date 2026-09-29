# The following boilerplate must be in this order for ESP-IDF CMake projects.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(SI_VERSION_BASE "0.87.6")
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "${SI_VERSION_BASE}-dev")
endif()
project(si_esphost_esp32p4)

# ESP-IDF 5.5.5 resets errno before each async response read, but its chunked
# and fixed-length body loops only classify EAGAIN as resumable. The TLS
# transport reports an ordinary poll timeout as
# ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT (0), so a healthy delayed response is
# otherwise mislabeled INCOMPLETE_DATA and the client closes or resets the
# connection. Generate a hash-guarded build copy that maps that exact async
# timeout to HTTP_EAGAIN before any terminal response cleanup. This keeps the
# same request/client alive and must not be emulated by resending the POST.
# HEAD and status codes that cannot carry a response body are excluded: their
# zero-length read is completion, not a transport wait.
idf_component_get_property(si_http_client_component esp_http_client COMPONENT_LIB)
idf_component_get_property(si_http_client_component_dir esp_http_client COMPONENT_DIR)
set(si_http_client_upstream
    "${si_http_client_component_dir}/esp_http_client.c")
set(si_http_client_555_sha256
    "8dec4bc7ff4b8a24b5e870786825edfc1ea6f5bd93afa3b049463646dd9aaf01")
file(SHA256 "${si_http_client_upstream}" si_http_client_actual_sha256)
if(NOT si_http_client_actual_sha256 STREQUAL si_http_client_555_sha256)
    message(FATAL_ERROR
        "esp_http_client.c changed; review the ExoAnchor async-read override")
endif()
file(READ "${si_http_client_upstream}" si_http_client_source)
set(si_http_async_read_upstream
"                        if (client->is_async && errno == EAGAIN) {
                            return ESP_ERR_HTTP_EAGAIN;
                        }")
set(si_http_async_read_fixed
"                        if (client->is_async &&
                            client->connection_info.method != HTTP_METHOD_HEAD &&
                            client->response->status_code / 100 != 1 &&
                            client->response->status_code != 204 &&
                            client->response->status_code != 304 &&
                            (errno == EAGAIN ||
                             ret == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT)) {
                            return ESP_ERR_HTTP_EAGAIN;
                        }")
string(FIND "${si_http_client_source}" "${si_http_async_read_upstream}"
       si_http_async_read_index)
if(si_http_async_read_index EQUAL -1)
    message(FATAL_ERROR
        "esp_http_client async response-read fragment not found")
endif()
string(REPLACE "${si_http_async_read_upstream}"
               "${si_http_async_read_fixed}"
               si_http_client_patched "${si_http_client_source}")
string(FIND "${si_http_client_patched}" "${si_http_async_read_upstream}"
       si_http_async_read_remaining)
if(NOT si_http_async_read_remaining EQUAL -1)
    message(FATAL_ERROR
        "esp_http_client async response-read override was incomplete")
endif()
set(si_http_client_patch_dir
    "${CMAKE_BINARY_DIR}/exoanchor-patched/esp_http_client")
file(MAKE_DIRECTORY "${si_http_client_patch_dir}")
set(si_http_client_patched_source
    "${si_http_client_patch_dir}/esp_http_client.c")
file(WRITE "${si_http_client_patched_source}" "${si_http_client_patched}")
get_target_property(si_http_client_sources ${si_http_client_component} SOURCES)
list(FIND si_http_client_sources "${si_http_client_upstream}"
     si_http_client_upstream_index)
if(si_http_client_upstream_index EQUAL -1)
    message(FATAL_ERROR "Could not replace esp_http_client source")
endif()
list(REMOVE_ITEM si_http_client_sources "${si_http_client_upstream}")
list(APPEND si_http_client_sources "${si_http_client_patched_source}")
set_property(TARGET ${si_http_client_component} PROPERTY SOURCES
             "${si_http_client_sources}")
message(STATUS "Using ExoAnchor ESP-IDF 5.5.5 async response-read fix")

# Preserve the non-Espressif license notices alongside every build so the
# Toolkit can include them in precompiled firmware release archives.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/THIRD_PARTY_NOTICES.txt"
    "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt"
    COPYONLY
)

# Keep usb_host_uvc on the registry implementation. The bounded cleanup,
# strict MS2109 assembly, and MJPEG quality negotiation overrides below are
# hash- and fragment-guarded so transport behaviour remains reviewable.
idf_component_get_property(si_uvc_component espressif__usb_host_uvc COMPONENT_LIB)
idf_component_get_property(si_uvc_component_dir espressif__usb_host_uvc COMPONENT_DIR)

# usb_host_uvc 2.5.1 has two fatal cleanup paths that are reachable when an
# isochronous capture stalls or a rapid reopen runs out of transfer memory:
# partial-transfer allocation is freed twice, and interface-release failures
# are wrapped in ESP_ERROR_CHECK. Generate a guarded build copy that makes
# transfer cleanup idempotent and returns release failures to the supervisor.
# A release failure must never fall through to freeing a stream that may still
# own in-flight URBs. Keep the registry component otherwise unchanged.
set(si_uvc_upstream_host "${si_uvc_component_dir}/uvc_host.c")
set(si_uvc_251_host_sha256
    "a9f3d4a94c95530b76f1ac84edfe3311617b272f2a947ac7bae72abc35949e4f")
file(SHA256 "${si_uvc_upstream_host}" si_uvc_upstream_host_sha256)
if(NOT si_uvc_upstream_host_sha256 STREQUAL si_uvc_251_host_sha256)
    message(FATAL_ERROR
        "usb_host_uvc/uvc_host.c changed; review the ExoAnchor cleanup override")
endif()

file(READ "${si_uvc_upstream_host}" si_uvc_host_source)
set(si_uvc_cache_state_upstream
"static uvc_host_driver_t *p_uvc_host_driver = NULL;")
set(si_uvc_cache_state_fixed [=[
static uvc_host_driver_t *p_uvc_host_driver = NULL;

/* ExoAnchor has one physical capture path. Keep one fully stopped URB ring
 * across a physical MS2109 close so reopen does not depend on coalescing the
 * internal-DMA heap. The ring is rebound only when its exact topology matches
 * and is always released when the UVC driver is uninstalled. */
typedef struct {
    usb_transfer_t **xfers;
    unsigned num_of_xfers;
    size_t transfer_size;
    int num_isoc_packets;
} uvc_transfer_cache_t;

static uvc_transfer_cache_t s_uvc_transfer_cache;
]=])
string(FIND "${si_uvc_host_source}" "${si_uvc_cache_state_upstream}"
       si_uvc_cache_state_index)
if(si_uvc_cache_state_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc driver state fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_cache_state_upstream}"
               "${si_uvc_cache_state_fixed}"
               si_uvc_host_source "${si_uvc_host_source}")

set(si_uvc_transfer_free_upstream
"    assert(uvc_stream);
    for (unsigned i = 0; i < uvc_stream->constant.num_of_xfers; i++) {
        usb_host_transfer_free(uvc_stream->constant.xfers[i]);
    }
    free(uvc_stream->constant.xfers);")
set(si_uvc_transfer_free_fixed [=[
    assert(uvc_stream);
    if (!uvc_stream->constant.xfers) {
        uvc_stream->constant.num_of_xfers = 0;
        return;
    }
    for (unsigned i = 0; i < uvc_stream->constant.num_of_xfers; i++) {
        if (uvc_stream->constant.xfers[i]) {
            usb_host_transfer_free(uvc_stream->constant.xfers[i]);
        }
    }
    free(uvc_stream->constant.xfers);
    uvc_stream->constant.xfers = NULL;
    uvc_stream->constant.num_of_xfers = 0;
}

static void uvc_transfer_cache_free(void)
{
    if (!s_uvc_transfer_cache.xfers) {
        return;
    }
    for (unsigned i = 0; i < s_uvc_transfer_cache.num_of_xfers; i++) {
        if (s_uvc_transfer_cache.xfers[i]) {
            usb_host_transfer_free(s_uvc_transfer_cache.xfers[i]);
        }
    }
    free(s_uvc_transfer_cache.xfers);
    memset(&s_uvc_transfer_cache, 0, sizeof(s_uvc_transfer_cache));
}

static bool uvc_transfer_cache_matches(unsigned num_of_transfers,
                                       size_t transfer_size,
                                       int num_isoc_packets)
{
    if (!s_uvc_transfer_cache.xfers ||
        s_uvc_transfer_cache.num_of_xfers != num_of_transfers ||
        s_uvc_transfer_cache.transfer_size != transfer_size ||
        s_uvc_transfer_cache.num_isoc_packets != num_isoc_packets) {
        return false;
    }
    for (unsigned i = 0; i < num_of_transfers; i++) {
        usb_transfer_t *transfer = s_uvc_transfer_cache.xfers[i];
        if (!transfer || transfer->data_buffer_size != transfer_size ||
            transfer->num_isoc_packets != num_isoc_packets) {
            return false;
        }
    }
    return true;
}

static void uvc_transfers_retain(uvc_stream_t *uvc_stream)
{
    assert(uvc_stream);
    if (!uvc_stream->constant.xfers ||
        uvc_stream->constant.num_of_xfers == 0) {
        return;
    }
    uvc_transfer_cache_free();
    s_uvc_transfer_cache.xfers = uvc_stream->constant.xfers;
    s_uvc_transfer_cache.num_of_xfers = uvc_stream->constant.num_of_xfers;
    s_uvc_transfer_cache.transfer_size =
        uvc_stream->constant.xfers[0]->data_buffer_size;
    s_uvc_transfer_cache.num_isoc_packets =
        uvc_stream->constant.xfers[0]->num_isoc_packets;
    for (unsigned i = 0; i < s_uvc_transfer_cache.num_of_xfers; i++) {
        s_uvc_transfer_cache.xfers[i]->device_handle = NULL;
        s_uvc_transfer_cache.xfers[i]->context = NULL;
    }
    uvc_stream->constant.xfers = NULL;
    uvc_stream->constant.num_of_xfers = 0;
    ESP_LOGI(TAG, "Retained %u stopped USB transfers for reopen",
             s_uvc_transfer_cache.num_of_xfers);
]=])
string(FIND "${si_uvc_host_source}" "${si_uvc_transfer_free_upstream}"
       si_uvc_transfer_free_index)
if(si_uvc_transfer_free_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc transfer cleanup fragment not found; review the override")
endif()
string(REPLACE "${si_uvc_transfer_free_upstream}"
               "${si_uvc_transfer_free_fixed}"
               si_uvc_host_patched "${si_uvc_host_source}")

set(si_uvc_transfer_array_upstream [=[
    // Allocate array of transfers
    uvc_stream->constant.xfers = malloc(num_of_transfers * sizeof(usb_transfer_t *));
    UVC_CHECK(uvc_stream->constant.xfers, ESP_ERR_NO_MEM);
]=])
set(si_uvc_transfer_array_fixed [=[
    // Reuse the complete stopped ring only when every immutable allocation
    // property matches. A topology change deliberately falls back to a fresh
    // allocation after releasing the incompatible cache.
    if (uvc_transfer_cache_matches(num_of_transfers, transfer_size,
                                   num_isoc_packets)) {
        uvc_stream->constant.xfers = s_uvc_transfer_cache.xfers;
        memset(&s_uvc_transfer_cache, 0, sizeof(s_uvc_transfer_cache));
        ESP_LOGI(TAG, "Reusing %u stopped USB transfers", num_of_transfers);
    } else {
        uvc_transfer_cache_free();
        uvc_stream->constant.xfers = calloc(
            num_of_transfers, sizeof(usb_transfer_t *));
        UVC_CHECK(uvc_stream->constant.xfers, ESP_ERR_NO_MEM);
    }
]=])
string(FIND "${si_uvc_host_patched}" "${si_uvc_transfer_array_upstream}"
       si_uvc_transfer_array_index)
if(si_uvc_transfer_array_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc transfer-array fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_transfer_array_upstream}"
               "${si_uvc_transfer_array_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

set(si_uvc_transfer_alloc_upstream [=[
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_alloc(transfer_size, num_isoc_packets, &uvc_stream->constant.xfers[i]),
            err, TAG, "Could not allocate USB transfers");
]=])
set(si_uvc_transfer_alloc_fixed [=[
        if (uvc_stream->constant.xfers[i] == NULL) {
            ESP_GOTO_ON_ERROR(
                usb_host_transfer_alloc(transfer_size, num_isoc_packets,
                                        &uvc_stream->constant.xfers[i]),
                err, TAG, "Could not allocate USB transfers");
        }
]=])
string(FIND "${si_uvc_host_patched}" "${si_uvc_transfer_alloc_upstream}"
       si_uvc_transfer_alloc_index)
if(si_uvc_transfer_alloc_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc transfer allocation fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_transfer_alloc_upstream}"
               "${si_uvc_transfer_alloc_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

set(si_uvc_remove_upstream [=[
static void uvc_device_remove(uvc_stream_t *uvc_stream)
{
    assert(uvc_stream);
    uvc_transfers_free(uvc_stream);
]=])
set(si_uvc_remove_fixed [=[
static void uvc_device_remove(uvc_stream_t *uvc_stream, bool retain_transfers)
{
    assert(uvc_stream);
    if (retain_transfers) {
        uvc_transfers_retain(uvc_stream);
    } else {
        uvc_transfers_free(uvc_stream);
    }
]=])
string(FIND "${si_uvc_host_patched}" "${si_uvc_remove_upstream}"
       si_uvc_remove_index)
if(si_uvc_remove_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc device-remove fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_remove_upstream}" "${si_uvc_remove_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

string(REPLACE
       "uvc_device_remove(uvc_stream); // Including transfers and frames free"
       "uvc_device_remove(uvc_stream, false); // Including transfers and frames free"
       si_uvc_host_patched "${si_uvc_host_patched}")
set(si_uvc_close_remove_upstream
"    uvc_device_remove(uvc_stream);

exit:")
set(si_uvc_close_remove_fixed
"    uvc_device_remove(uvc_stream, true);

exit:")
string(FIND "${si_uvc_host_patched}" "${si_uvc_close_remove_upstream}"
       si_uvc_close_remove_index)
if(si_uvc_close_remove_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc close-remove fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_close_remove_upstream}"
               "${si_uvc_close_remove_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

set(si_uvc_uninstall_cache_upstream
"    ESP_LOGD(TAG, \"Deregistering client\");
    ESP_ERROR_CHECK(usb_host_client_deregister(uvc_obj->usb_client_hdl));")
set(si_uvc_uninstall_cache_fixed
"    uvc_transfer_cache_free();
    ESP_LOGD(TAG, \"Deregistering client\");
    ESP_ERROR_CHECK(usb_host_client_deregister(uvc_obj->usb_client_hdl));")
string(FIND "${si_uvc_host_patched}" "${si_uvc_uninstall_cache_upstream}"
       si_uvc_uninstall_cache_index)
if(si_uvc_uninstall_cache_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc uninstall fragment not found; review the URB cache override")
endif()
string(REPLACE "${si_uvc_uninstall_cache_upstream}"
               "${si_uvc_uninstall_cache_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

set(si_uvc_release_upstream
"    // Release all interfaces
    ESP_ERROR_CHECK(usb_host_interface_release(p_uvc_host_driver->usb_client_hdl, uvc_stream->constant.dev_hdl, uvc_stream->constant.bInterfaceNumber));")
set(si_uvc_release_fixed
"    // Do not free the stream after any interface-release failure. In
    // particular, INVALID_STATE can mean transfers still reference it.
    esp_err_t release_ret = usb_host_interface_release(
        p_uvc_host_driver->usb_client_hdl, uvc_stream->constant.dev_hdl,
        uvc_stream->constant.bInterfaceNumber);
    if (release_ret != ESP_OK) {
        ESP_LOGW(TAG, \"Interface release returned %s\",
                 esp_err_to_name(release_ret));
        ret = release_ret;
        goto exit;
    }")
string(FIND "${si_uvc_host_patched}" "${si_uvc_release_upstream}"
       si_uvc_release_index)
if(si_uvc_release_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc interface-release fragment not found; review the override")
endif()
string(REPLACE "${si_uvc_release_upstream}" "${si_uvc_release_fixed}"
               si_uvc_host_patched "${si_uvc_host_patched}")

set(si_uvc_patch_dir "${CMAKE_BINARY_DIR}/exoanchor-patched/usb_host_uvc")
file(MAKE_DIRECTORY "${si_uvc_patch_dir}")
set(si_uvc_patched_host "${si_uvc_patch_dir}/uvc_host.c")
file(WRITE "${si_uvc_patched_host}" "${si_uvc_host_patched}")
get_target_property(si_uvc_sources ${si_uvc_component} SOURCES)
list(FIND si_uvc_sources "${si_uvc_upstream_host}" si_uvc_upstream_host_index)
if(si_uvc_upstream_host_index EQUAL -1)
    message(FATAL_ERROR "Could not replace usb_host_uvc host source")
endif()
list(REMOVE_ITEM si_uvc_sources "${si_uvc_upstream_host}")
list(APPEND si_uvc_sources "${si_uvc_patched_host}")
set_property(TARGET ${si_uvc_component} PROPERTY SOURCES "${si_uvc_sources}")
message(STATUS "Using ExoAnchor usb_host_uvc cleanup safety fixes")

# usb_host_uvc 2.5.1 already performs the UVC-mandated GET_MAX/GET_MIN Probe
# requests, but discards wCompQuality and commits a zero-filled preference.
# That leaves high-entropy MS2109 frames large enough to saturate its 3072-byte
# high-speed isochronous payload before 25 fps. Replace only the complete
# commit function in a guarded build copy. An explicitly selected frame rate
# sets bmHint.D0 so the device keeps the interval ahead of compression quality.
# The ExoAnchor 1..100 quality setting is mapped and bmHint.D3 is set only when
# GET_MIN/GET_MAX prove that wCompQuality is supported; zero is the UVC-defined
# unsupported value and must not be replaced by a guessed range. The registry
# source itself stays untouched.
set(si_uvc_upstream_control "${si_uvc_component_dir}/uvc_control.c")
set(si_uvc_251_control_sha256
    "a472df7d6aee8eb7b03b21c5e9d2f97edec910d3a292a64dae5e3bf0b698c754")
file(SHA256 "${si_uvc_upstream_control}" si_uvc_control_actual_sha256)
if(NOT si_uvc_control_actual_sha256 STREQUAL si_uvc_251_control_sha256)
    message(FATAL_ERROR
        "usb_host_uvc/uvc_control.c changed; review the MJPEG quality override")
endif()
file(READ "${si_uvc_upstream_control}" si_uvc_control_source)
set(si_uvc_commit_upstream [=[
esp_err_t uvc_host_stream_control_commit(uvc_host_stream_hdl_t stream_hdl, const uvc_host_stream_format_t *vs_format)
{
    /*
    This is not a 'real' negotiation as we return OK only if set the format exactly as expected.
    In future, we can make vs_format parameter inout. This way we would return the real (close enough) set VS format.
    */
    UVC_CHECK(stream_hdl && vs_format, ESP_ERR_INVALID_ARG);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uvc_vs_ctrl_t vs_result = {0};
    uvc_vs_ctrl_t vs_result_ignored = {0};
    uvc_host_stream_format_t format_set, format_ignored;

    // Try 2x. Some camera may return error on first try
    for (int i = 0; i < 2; i++) {
        // Notes: Some cameras require 'probe_get' to be the 1st negotiation call
        // It returns 'default' format and frame settings. It is ignored for now.
        // We can reuse the returned value, if we wanted to implement 'negotiate default frame format' feature.
        memcpy(&format_set, vs_format, sizeof(uvc_host_stream_format_t));
        uvc_host_stream_control_probe(stream_hdl, &format_set, NULL);

        // We do this to mimic Windows driver: The Min/Max values are ignored by this driver.
        // These values can be used if we wanted to negotiate advanced parameters, such as wCompQuality to select JPEG encoding quality
        uvc_control_probe_get_max(stream_hdl, &vs_result_ignored, &format_ignored);
        uvc_control_probe_get_min(stream_hdl, &vs_result_ignored, &format_ignored);

        // Probe that the camera accepts our format before committing
        ret = uvc_control_probe_set(stream_hdl, &vs_result, vs_format);
        ret |= uvc_control_probe_get(stream_hdl, &vs_result, &format_set);
        UVC_CHECK(ret == ESP_OK, ret);
        if (uvc_is_vs_format_equal(&format_set, vs_format)) {
            // If the 'set format' equals 'get format', the camera accepts our format and we can commit it
            break;
        }
    }

    ESP_LOGD(TAG, "Frame format negotiation:\n\tRequested: %dx%d@%2.1fFPS\n\tGot: %dx%d@%2.1fFPS",
             vs_format->h_res, vs_format->v_res, vs_format->fps, format_set.h_res, format_set.v_res, format_set.fps);

    // Commit the negotiated format
    return uvc_control_commit(stream_hdl, &vs_result, vs_format);
}
]=])
set(si_uvc_commit_fixed [=[
esp_err_t uvc_host_stream_control_commit(uvc_host_stream_hdl_t stream_hdl, const uvc_host_stream_format_t *vs_format)
{
    UVC_CHECK(stream_hdl && vs_format, ESP_ERR_INVALID_ARG);
    extern uint8_t si_uvc_mjpeg_quality_percent(void);
    const uint8_t requested_quality = si_uvc_mjpeg_quality_percent();
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uvc_vs_ctrl_t vs_result = {0};
    uvc_vs_ctrl_t vs_min = {0};
    uvc_vs_ctrl_t vs_max = {0};
    uvc_host_stream_format_t format_set = {0};
    uvc_host_stream_format_t format_min = {0};
    uvc_host_stream_format_t format_max = {0};
    const uint16_t frame_interval_hint = (1U << 0);
    const uint16_t compression_quality_hint = (1U << 3);

    for (int i = 0; i < 2; i++) {
        memcpy(&format_set, vs_format, sizeof(uvc_host_stream_format_t));
        ret = uvc_host_stream_control_probe(stream_hdl, &format_set,
                                            &vs_result);
        UVC_CHECK(ret == ESP_OK, ret);

        const esp_err_t max_ret =
            uvc_control_probe_get_max(stream_hdl, &vs_max, &format_max);
        const esp_err_t min_ret =
            uvc_control_probe_get_min(stream_hdl, &vs_min, &format_min);
        const uint16_t current_quality = vs_result.wCompQuality;

        /* UVC 1.5 table 4-75: bmHint marks the fields that shall remain
         * constant during negotiation. Prefer the explicitly requested frame
         * interval; never bias the device toward an unsupported guessed JPEG
         * quality range. Preserve unrelated hints returned by the device. */
        vs_result.bmHint &=
            (uint16_t)~(frame_interval_hint | compression_quality_hint);
        if (vs_format->fps > 0.0f) {
            vs_result.bmHint |= frame_interval_hint;
        }
        vs_result.wCompQuality = 0;

        const bool quality_supported =
            requested_quality != 0 && requested_quality <= 100 &&
            vs_format->format == UVC_VS_FORMAT_MJPEG &&
            min_ret == ESP_OK && max_ret == ESP_OK &&
            vs_min.wCompQuality > 0 &&
            vs_max.wCompQuality >= vs_min.wCompQuality;
        if (quality_supported) {
            const uint16_t quality_min = vs_min.wCompQuality;
            const uint16_t quality_max = vs_max.wCompQuality;
            const uint32_t span = (uint32_t)quality_max - quality_min;
            const uint16_t target = (uint16_t)(quality_min +
                ((uint32_t)(requested_quality - 1U) * span) / 99U);
            vs_result.bmHint |= compression_quality_hint;
            vs_result.wCompQuality = target;
            ESP_LOGI(TAG,
                     "MJPEG quality Probe: request=%u%% current=%u target=%u min=%u max=%u payload=%"PRIu32,
                     requested_quality, current_quality, target, quality_min,
                     quality_max, vs_result.dwMaxPayloadTransferSize);
        } else if (requested_quality != 0 &&
                   vs_format->format == UVC_VS_FORMAT_MJPEG) {
            ESP_LOGI(TAG,
                     "MJPEG quality unsupported: request=%u%% min=%u max=%u; preserving frame interval hint",
                     requested_quality, vs_min.wCompQuality,
                     vs_max.wCompQuality);
        }

        ret = uvc_control_probe_set(stream_hdl, &vs_result, vs_format);
        ret |= uvc_control_probe_get(stream_hdl, &vs_result, &format_set);
        UVC_CHECK(ret == ESP_OK, ret);
        ESP_LOGI(TAG,
                 "UVC Probe accepted: interval=%"PRIu32" hint=0x%04x quality=%u frame=%"PRIu32" payload=%"PRIu32,
                 vs_result.dwFrameInterval, vs_result.bmHint,
                 vs_result.wCompQuality, vs_result.dwMaxVideoFrameSize,
                 vs_result.dwMaxPayloadTransferSize);
        if (uvc_is_vs_format_equal(&format_set, vs_format)) {
            break;
        }
    }

    ESP_LOGD(TAG, "Frame format negotiation:\n\tRequested: %dx%d@%2.1fFPS\n\tGot: %dx%d@%2.1fFPS",
             vs_format->h_res, vs_format->v_res, vs_format->fps,
             format_set.h_res, format_set.v_res, format_set.fps);
    return uvc_control_commit(stream_hdl, &vs_result, vs_format);
}
]=])
string(FIND "${si_uvc_control_source}" "${si_uvc_commit_upstream}"
       si_uvc_commit_index)
if(si_uvc_commit_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc commit fragment not found; review the MJPEG quality override")
endif()
string(REPLACE "${si_uvc_commit_upstream}" "${si_uvc_commit_fixed}"
               si_uvc_control_patched "${si_uvc_control_source}")
set(si_uvc_patched_control "${si_uvc_patch_dir}/uvc_control.c")
file(WRITE "${si_uvc_patched_control}" "${si_uvc_control_patched}")
get_target_property(si_uvc_sources ${si_uvc_component} SOURCES)
list(FIND si_uvc_sources "${si_uvc_upstream_control}"
     si_uvc_upstream_control_index)
if(si_uvc_upstream_control_index EQUAL -1)
    message(FATAL_ERROR "Could not replace usb_host_uvc control source")
endif()
list(REMOVE_ITEM si_uvc_sources "${si_uvc_upstream_control}")
list(APPEND si_uvc_sources "${si_uvc_patched_control}")
set_property(TARGET ${si_uvc_component} PROPERTY SOURCES "${si_uvc_sources}")
message(STATUS "Using ExoAnchor UVC MJPEG quality negotiation override")

# MS2109 toggles the UVC Frame ID for every MJPEG image but omits the EOF bit
# in its payload headers.  Frame ID is itself a UVC frame-boundary signal; the
# registry driver currently treats a toggle without EOF as an unconditional
# damaged-frame reset, so this standards-compatible camera can never deliver a
# frame.  Keep the upstream assembler and change only that boundary case: a
# previous MJPEG buffer is delivered on the FID transition iff it already ends
# in an exact JPEG EOI marker.  No byte scanning, trimming, concatenation, or
# synthetic marker insertion is allowed; the application validator remains
# responsible for the complete JPEG structural check before publication.
set(si_uvc_upstream_isoc "${si_uvc_component_dir}/uvc_isoc.c")
set(si_uvc_251_isoc_sha256
    "c709685f2814697c568620c7c2b7974e3e3167a92308ddd324c46db082de1ccc")
file(SHA256 "${si_uvc_upstream_isoc}" si_uvc_isoc_actual_sha256)
if(NOT si_uvc_isoc_actual_sha256 STREQUAL si_uvc_251_isoc_sha256)
    message(FATAL_ERROR
        "usb_host_uvc/uvc_isoc.c changed; review the MS2109 FID-boundary fix")
endif()

file(READ "${si_uvc_upstream_isoc}" si_uvc_isoc_source)

# The registry component treats a skipped/timed-out isochronous packet as
# harmless. USB Host defines SKIPPED as system-latency/bus-overload loss, so an
# incomplete active frame must be tainted. MS2109, however, omits EOF and leaves
# a complete JPEG (ending FF D9) attached until the next FID boundary; idle
# SKIPPED descriptors in that interval are not missing JPEG payload and must not
# invalidate the already complete frame.
set(si_uvc_loss_upstream [=[
        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
]=])
set(si_uvc_loss_fixed [=[
        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED: {
            uvc_host_frame_t *loss_frame =
                UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
            const bool completed_mjpeg_waiting_for_fid =
                loss_frame &&
                uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                loss_frame->data_len >= 2 &&
                loss_frame->data[loss_frame->data_len - 2] == JPEG_MARKER &&
                loss_frame->data[loss_frame->data_len - 1] == 0xD9U;
            if (loss_frame && !completed_mjpeg_waiting_for_fid) {
                uvc_stream->single_thread.skip_current_frame = true;
            }
            goto next_isoc_packet; // Taint only an incomplete active frame
        }
]=])
string(FIND "${si_uvc_isoc_source}" "${si_uvc_loss_upstream}"
       si_uvc_loss_index)
if(si_uvc_loss_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc packet-loss fragment not found; review the override")
endif()
string(REPLACE "${si_uvc_loss_upstream}" "${si_uvc_loss_fixed}"
               si_uvc_isoc_loss_patched "${si_uvc_isoc_source}")

set(si_uvc_sof_upstream [=[
        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;

            // Check mjpeg frame start
            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                    (payload_data[0] != JPEG_MARKER || payload_data[1] != JPEG_SOI)) {
                // We received frame with invalid frame, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;
                ESP_LOGW(TAG, "invalid MJPEG SOI");
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->single_thread.skip_current_frame = true;

                    // Inform the user about the underflow
                    uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                    if (stream_cb) {
                        const uvc_host_stream_event_data_t event = {
                            .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                        };
                        stream_cb(&event, uvc_stream->constant.cb_arg);
                    }
                    goto next_isoc_packet;
                }
            } else {
                // We received SoF but current_frame is not NULL: We missed EoF - reset the frame buffer
                ESP_EARLY_LOGW(TAG, "missed EoF");
                uvc_stream->single_thread.skip_current_frame = true;
                uvc_frame_reset(uvc_stream->dynamic.current_frame);
                UVC_EXIT_CRITICAL();
            }
        }
]=])
set(si_uvc_sof_fixed [=[
        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            /* FID is a frame boundary even when a camera omits EOF. Detach the
             * previous buffer first so its ownership is identical to the
             * normal EOF path. Only an exact terminal EOI is eligible for the
             * callback; incomplete or skipped frames are returned untouched. */
            uvc_host_frame_t *previous_frame = NULL;
            bool previous_frame_complete = false;
            UVC_ENTER_CRITICAL();
            previous_frame = uvc_stream->dynamic.current_frame;
            if (previous_frame) {
                uvc_stream->dynamic.current_frame = NULL;
                previous_frame_complete =
                    !uvc_stream->single_thread.skip_current_frame &&
                    uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                    previous_frame->data_len >= 4 &&
                    previous_frame->data[0] == JPEG_MARKER &&
                    previous_frame->data[1] == JPEG_SOI &&
                    previous_frame->data[previous_frame->data_len - 2] == JPEG_MARKER &&
                    previous_frame->data[previous_frame->data_len - 1] == 0xD9U;
            }
            UVC_EXIT_CRITICAL();

            if (previous_frame) {
                bool return_previous = true;
                if (previous_frame_complete &&
                        UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming) &&
                        uvc_stream->constant.frame_cb) {
                    return_previous = uvc_stream->constant.frame_cb(
                        previous_frame, uvc_stream->constant.cb_arg);
                }
                if (return_previous) {
                    uvc_host_frame_return(uvc_stream, previous_frame);
                }
            }

            // Start the new FID with state derived only from its own header.
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;

            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                    (payload_data_len < 2 || payload_data[0] != JPEG_MARKER ||
                     payload_data[1] != JPEG_SOI)) {
                uvc_stream->single_thread.skip_current_frame = true;
                ESP_LOGW(TAG, "invalid MJPEG SOI");
            }

            UVC_ENTER_CRITICAL();
            const bool need_new_frame =
                uvc_stream->dynamic.streaming &&
                !uvc_stream->dynamic.current_frame;
            UVC_EXIT_CRITICAL();
            if (!need_new_frame) {
                uvc_stream->single_thread.skip_current_frame = true;
                goto next_isoc_packet;
            }

            uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
            if (uvc_stream->dynamic.current_frame == NULL) {
                uvc_stream->single_thread.skip_current_frame = true;
                uvc_host_stream_callback_t stream_cb =
                    uvc_stream->constant.stream_cb;
                if (stream_cb) {
                    const uvc_host_stream_event_data_t event = {
                        .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                    };
                    stream_cb(&event, uvc_stream->constant.cb_arg);
                }
                goto next_isoc_packet;
            }
        }
]=])
string(FIND "${si_uvc_isoc_loss_patched}" "${si_uvc_sof_upstream}"
       si_uvc_sof_index)
if(si_uvc_sof_index EQUAL -1)
    message(FATAL_ERROR
        "usb_host_uvc FID-boundary fragment not found; review the override")
endif()
string(REPLACE "${si_uvc_sof_upstream}" "${si_uvc_sof_fixed}"
               si_uvc_isoc_patched "${si_uvc_isoc_loss_patched}")
set(si_uvc_patched_isoc "${si_uvc_patch_dir}/uvc_isoc.c")
file(WRITE "${si_uvc_patched_isoc}" "${si_uvc_isoc_patched}")
get_target_property(si_uvc_sources ${si_uvc_component} SOURCES)
list(FIND si_uvc_sources "${si_uvc_upstream_isoc}"
     si_uvc_upstream_isoc_index)
if(si_uvc_upstream_isoc_index EQUAL -1)
    message(FATAL_ERROR "Could not replace usb_host_uvc isochronous source")
endif()
list(REMOVE_ITEM si_uvc_sources "${si_uvc_upstream_isoc}")
list(APPEND si_uvc_sources "${si_uvc_patched_isoc}")
set_property(TARGET ${si_uvc_component} PROPERTY SOURCES "${si_uvc_sources}")
message(STATUS "Using strict MS2109 FID-boundary compatibility fix")

# esp_h264 1.3.7 conservatively writes back the entire YUV input and
# invalidates the entire advertised output capacity for every frame.  In this
# product the H.264 input is exclusively the cache-invalidated output of the
# JPEG DMA engine, so the input writeback is redundant.  The bitstream DMA
# length is known after FRAME_DONE; invalidate that produced range, rounded to
# the P4 L2 cache line, while still advertising the full 4 MiB safety window to
# hardware.  Guard the exact upstream file so this ownership contract is
# re-reviewed on every component upgrade.
if(CONFIG_SI_VIDEO_H264_EXPERIMENT)
if(NOT CONFIG_CACHE_L2_CACHE_LINE_SIZE EQUAL 128)
    message(FATAL_ERROR
        "The esp_h264 DMA ownership override requires a 128-byte P4 L2 cache line")
endif()
if(NOT CONFIG_CACHE_L1_CACHE_LINE_SIZE EQUAL 64)
    message(FATAL_ERROR
        "The esp_h264 internal reference workspace requires a 64-byte P4 L1 cache line")
endif()
idf_component_get_property(si_h264_component espressif__esp_h264 COMPONENT_LIB)
idf_component_get_property(si_h264_component_dir espressif__esp_h264 COMPONENT_DIR)
set(si_h264_upstream_single
    "${si_h264_component_dir}/hw/src/esp_h264_enc_single_hw.c")
set(si_h264_upstream_param
    "${si_h264_component_dir}/hw/src/esp_h264_enc_hw_param.c")
set(si_h264_upstream_rc
    "${si_h264_component_dir}/hw/src/h264_rc.c")
set(si_h264_upstream_nal
    "${si_h264_component_dir}/hw/src/h264_nal.c")
set(si_h264_upstream_cache
    "${si_h264_component_dir}/port/src/esp_h264_cache.c")
set(si_h264_137_single_sha256
    "308525ae2f789732b78d269ab518f5f3e786e96fe115811d78e855c5cab4d2b7")
set(si_h264_137_param_sha256
    "cc9ca9aa5e58c394911b08f777719bd82b95acaa01947059c8da0cd7d4bdea42")
set(si_h264_137_rc_sha256
    "a1efc32b61f1742cc15d862eb970b87ea41d834ae86e6d3f49edea8cc1621aff")
set(si_h264_137_nal_sha256
    "7cb83c1331903ad482c784a3567e8253ff2ebb31d6eb9828e64ba538f64749f2")
set(si_h264_137_cache_sha256
    "ad3a681f1ba9c97e82ac01d9deb3eef3e8a42eecf9965af325ecd940255f8d48")
file(SHA256 "${si_h264_upstream_single}" si_h264_single_actual_sha256)
if(NOT si_h264_single_actual_sha256 STREQUAL si_h264_137_single_sha256)
    message(FATAL_ERROR
        "esp_h264 single-HW encoder changed; review the DMA ownership override")
endif()
file(SHA256 "${si_h264_upstream_param}" si_h264_param_actual_sha256)
if(NOT si_h264_param_actual_sha256 STREQUAL si_h264_137_param_sha256)
    message(FATAL_ERROR
        "esp_h264 HW parameter allocator changed; review the workspace reuse override")
endif()
file(SHA256 "${si_h264_upstream_rc}" si_h264_rc_actual_sha256)
if(NOT si_h264_rc_actual_sha256 STREQUAL si_h264_137_rc_sha256)
    message(FATAL_ERROR
        "esp_h264 HW rate control changed; review the 1.3.7 CLIP3 fix")
endif()
file(SHA256 "${si_h264_upstream_nal}" si_h264_nal_actual_sha256)
if(NOT si_h264_nal_actual_sha256 STREQUAL si_h264_137_nal_sha256)
    message(FATAL_ERROR
        "esp_h264 H.264 NAL writer changed; remove or re-review the SPS escaping fix")
endif()
file(SHA256 "${si_h264_upstream_cache}" si_h264_cache_actual_sha256)
if(NOT si_h264_cache_actual_sha256 STREQUAL si_h264_137_cache_sha256)
    message(FATAL_ERROR
        "esp_h264 cache wrapper changed; remove or re-review the cache-address guard")
endif()

# esp_h264 1.3.7 calls esp_cache_msync() for every allocation made through
# its aligned allocator.  On ESP32-P4, MALLOC_CAP_INTERNAL also includes SPM
# and RTCRAM; neither range is cache-backed, so there is no cache line to write
# back or invalidate for a complete range placed there.  DMA-role reachability
# remains a separate allocator/driver contract.  Skip only complete ranges in
# the two known uncached internal regions.  Every other address still reaches
# IDF's validator so a bad pointer, overflow, or cache-boundary violation
# remains a visible fault rather than being silently swallowed.
set(si_h264_cache_guard_fixed [=[
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_h264_cache.h"
#include "esp_memory_utils.h"
#include "soc/soc_caps.h"

static bool esp_h264_cache_range_is_known_uncached_internal(
    const uint8_t *addr, uint32_t length)
{
    if (!addr || length == 0U) {
        return false;
    }

    uintptr_t first = (uintptr_t)addr;
    uintptr_t last = 0;
    if (__builtin_add_overflow(first, (uintptr_t)length - 1U, &last)) {
        return false;
    }

#if SOC_RTC_FAST_MEM_SUPPORTED
    if (esp_ptr_in_rtc_dram_fast(addr) &&
        esp_ptr_in_rtc_dram_fast((const void *)last)) {
        return true;
    }
#endif
#if SOC_MEM_SPM_SUPPORTED
    if (esp_ptr_in_spm(addr) && esp_ptr_in_spm((const void *)last)) {
        return true;
    }
#endif
    return false;
}

void esp_h264_cache_check_and_writeback(uint8_t *addr, uint32_t length)
{
    if (esp_h264_cache_range_is_known_uncached_internal(addr, length)) {
        return;
    }
    (void)esp_cache_msync(addr, length,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void esp_h264_cache_check_and_invalidate(uint8_t *addr, uint32_t length)
{
    if (esp_h264_cache_range_is_known_uncached_internal(addr, length)) {
        return;
    }
    (void)esp_cache_msync(addr, length,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}
]=])

# esp_h264 1.3.7 added VUI timing_info but writes the RBSP directly into an
# Annex-B NAL.  num_units_in_tick=1 therefore emits 00 00 00 01 inside the
# SPS, which a standards-compliant reader treats as a new start code.  Escape
# only the SPS RBSP after its NAL header; fail rather than advertise a partial
# SPS if the caller-provided buffer cannot hold the prevention bytes.
file(READ "${si_h264_upstream_nal}" si_h264_nal_source)
set(si_h264_sps_escape_upstream [=[
    bs_rbsp_trailing(&bs);
    return nal_bs_size(&bs) + 32;
}

uint16_t esp_h264_enc_set_pps]=])
set(si_h264_sps_escape_fixed [=[
    bs_rbsp_trailing(&bs);
    uint16_t rbsp_bytes = (uint16_t)((nal_bs_size(&bs) + 7) >> 3);
    if ((uint32_t)rbsp_bytes + 4U > len) {
        return 0;
    }

    uint16_t escaped_bytes = rbsp_bytes;
    uint8_t zero_run = 0;
    /* Skip byte zero: it is the NAL header, not RBSP payload. */
    for (uint16_t index = 1; index < escaped_bytes; index++) {
        uint8_t value = buffer[4U + index];
        if (zero_run >= 2U && value <= 3U) {
            if ((uint32_t)escaped_bytes + 5U > len) {
                return 0;
            }
            memmove(buffer + 5U + index, buffer + 4U + index,
                    (size_t)(escaped_bytes - index));
            buffer[4U + index] = 0x03;
            escaped_bytes++;
            zero_run = 0;
            continue;
        }
        zero_run = value == 0U ? (uint8_t)(zero_run + 1U) : 0U;
    }
    return (uint16_t)((escaped_bytes + 4U) * 8U);
}

uint16_t esp_h264_enc_set_pps]=])
string(FIND "${si_h264_nal_source}" "${si_h264_sps_escape_upstream}"
       si_h264_sps_escape_index)
if(si_h264_sps_escape_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 SPS tail changed; remove or re-review the VUI escaping fix")
endif()
string(REPLACE "${si_h264_sps_escape_upstream}"
               "${si_h264_sps_escape_fixed}"
               si_h264_nal_patched "${si_h264_nal_source}")

file(READ "${si_h264_upstream_single}" si_h264_single_source)
set(si_h264_output_contract_upstream
"static esp_h264_err_t h264_hw_enc_gop_mode_process(esp_h264_hw_handle_t *hw_hd, uint8_t *in_frame, uint8_t *out_frame, uint32_t out_frame_size, uint32_t *out_len)
{")
set(si_h264_output_contract_fixed
"static esp_h264_err_t h264_hw_enc_gop_mode_process(esp_h264_hw_handle_t *hw_hd, uint8_t *in_frame, uint8_t *out_frame, uint32_t out_frame_size, uint32_t *out_len)
{
    if ((((uintptr_t)out_frame | (uintptr_t)out_frame_size) &
         (CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1U)) != 0U) {
        ESP_H264_LOGE(TAG, \"ExoAnchor output buffer violates the 128-byte cache ownership contract\");
        return ESP_H264_ERR_ARG;
    }")
string(FIND "${si_h264_single_source}" "${si_h264_output_contract_upstream}"
       si_h264_output_contract_index)
if(si_h264_output_contract_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 output contract insertion point not found; review the override")
endif()
string(REPLACE "${si_h264_output_contract_upstream}"
               "${si_h264_output_contract_fixed}"
               si_h264_single_patched "${si_h264_single_source}")

set(si_h264_input_sync_upstream
"    esp_h264_cache_check_and_writeback(in_frame->raw_data.buffer, in_frame->raw_data.len);")
set(si_h264_input_sync_fixed
"    /* ExoAnchor supplies JPEG-DMA-owned YUV that the CPU has not modified.
     * jpeg_decoder_process() already completed the M2C ownership transfer. */")
string(FIND "${si_h264_single_patched}" "${si_h264_input_sync_upstream}"
       si_h264_input_sync_index)
if(si_h264_input_sync_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 input cache-sync fragment not found; review the override")
endif()
string(REPLACE "${si_h264_input_sync_upstream}"
               "${si_h264_input_sync_fixed}"
               si_h264_single_patched "${si_h264_single_patched}")

set(si_h264_output_sync_upstream
"    esp_h264_cache_check_and_invalidate(out_frame, out_frame_size);")
set(si_h264_output_sync_fixed
"    if (out_frame_len < 0 || *out_len > out_frame_size ||
        (uint32_t)out_frame_len > out_frame_size - *out_len) {
        ESP_H264_LOGE(TAG, \"Encoded output exceeds the advertised buffer capacity\");
        return ESP_H264_ERR_MEM;
    }
    uint32_t produced_len = (uint32_t)out_frame_len + *out_len;
    uint32_t cache_len = (produced_len + CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1U) &
                         ~(CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1U);
    esp_h264_cache_check_and_invalidate(out_frame, cache_len);")
string(FIND "${si_h264_single_patched}" "${si_h264_output_sync_upstream}"
       si_h264_output_sync_index)
if(si_h264_output_sync_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 output cache-sync fragment not found; review the override")
endif()
string(REPLACE "${si_h264_output_sync_upstream}"
               "${si_h264_output_sync_fixed}"
               si_h264_single_patched "${si_h264_single_patched}")

# The ESP32-P4 encoder allocates a roughly 3.13 MiB deblocking workspace for
# 1080p.  Deleting a handle and later recreating 1080p after a 720p session can
# leave enough total PSRAM but no sufficiently large contiguous block.  Keep
# exactly one handle, as required by the single global register bank, while
# retaining the largest reference/deblocking allocations across transactional
# handle replacement.  A fresh parameter handle still rebuilds all geometry,
# SPS/PPS, rate-control, HAL, and descriptor state for every resolution.
file(READ "${si_h264_upstream_param}" si_h264_param_source)
set(si_h264_workspace_fields_upstream [=[
    uint8_t                   *db;
    uint8_t                   *ref;
    h264_dma_desc_t           *dsc_ref;]=])
set(si_h264_workspace_fields_fixed [=[
    uint8_t                   *db;
    uint32_t                   db_capacity;
    uint8_t                   *ref;
    uint32_t                   ref_capacity;
    h264_dma_desc_t           *dsc_ref;]=])
string(FIND "${si_h264_param_source}" "${si_h264_workspace_fields_upstream}"
       si_h264_workspace_fields_index)
if(si_h264_workspace_fields_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 workspace capacity insertion point not found; review the override")
endif()
string(REPLACE "${si_h264_workspace_fields_upstream}"
               "${si_h264_workspace_fields_fixed}"
               si_h264_param_patched "${si_h264_param_source}")

set(si_h264_internal_residency_include_upstream [=[
#include "esp_h264_alloc.h"
#include "esp_h264_enc_hw_param.h"]=])
set(si_h264_internal_residency_include_fixed [=[
#include "esp_h264_alloc.h"
#include "esp_memory_utils.h"
#include "esp_h264_enc_hw_param.h"]=])
string(FIND "${si_h264_param_patched}"
       "${si_h264_internal_residency_include_upstream}"
       si_h264_internal_residency_include_index)
if(si_h264_internal_residency_include_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 internal-residency include fragment not found; review the override")
endif()
string(REPLACE "${si_h264_internal_residency_include_upstream}"
               "${si_h264_internal_residency_include_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_workspace_helpers_upstream [=[
} esp_h264_param_t;

#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300]=])
set(si_h264_workspace_helpers_fixed [=[
} esp_h264_param_t;

static esp_h264_err_t build_parameter_nals(uint8_t *buffer,
                                           uint8_t buffer_len,
                                           uint16_t height,
                                           uint16_t width,
                                           uint8_t fps,
                                           uint8_t qp,
                                           uint16_t *out_bit_len)
{
    if (!buffer || !out_bit_len || buffer_len == 0U) {
        return ESP_H264_ERR_ARG;
    }
    *out_bit_len = 0;
    memset(buffer, 0, buffer_len);
    uint16_t sps_bits = esp_h264_enc_set_sps(buffer, buffer_len, height,
                                             width, fps);
    if (sps_bits == 0U || (sps_bits & 7U) != 0U ||
        (sps_bits >> 3) >= buffer_len) {
        return ESP_H264_ERR_OVERFLOW;
    }
    uint16_t pps_bits = esp_h264_enc_set_pps(
        buffer + (sps_bits >> 3), buffer_len - (sps_bits >> 3), qp, true);
    uint32_t total_bits = (uint32_t)sps_bits + pps_bits;
    if (pps_bits == 0U || (pps_bits & 7U) != 0U ||
        total_bits > (uint32_t)buffer_len * 8U) {
        return ESP_H264_ERR_OVERFLOW;
    }
    *out_bit_len = (uint16_t)total_bits;
    return ESP_H264_ERR_OK;
}

/* ExoAnchor serializes all single-HW encoder ownership with its codec gate.
 * Retaining these two largest workspaces avoids fragmentation on
 * 1080p -> 720p -> 1080p transitions without keeping two hardware handles
 * alive against one global H.264 register bank. */
static uint8_t *s_cached_ref_workspace;
static uint32_t s_cached_ref_capacity;
static uint8_t *s_cached_db_workspace;
static uint32_t s_cached_db_capacity;

/* The reference workspace deliberately uses Espressif's allocator with the
 * strict INTERNAL capability.  The much larger DB workspace keeps the
 * upstream esp_h264_malloc_prefer(INTERNAL, SPIRAM) policy below. */

static uint8_t *take_cached_workspace(uint8_t **cached,
                                      uint32_t *cached_capacity,
                                      uint32_t required,
                                      uint32_t *actual_size,
                                      const char *name)
{
    if (*cached && *cached_capacity >= required) {
        uint8_t *workspace = *cached;
        *cached = NULL;
        *actual_size = *cached_capacity;
        *cached_capacity = 0;
        /* esp_h264 1.3.7 intentionally changed these large workspaces from
         * calloc to malloc: the encoder overwrites them before use.  Keep the
         * same non-zeroing contract when a cached allocation is reused. */
        esp_h264_cache_check_and_writeback(workspace, *actual_size);
        ESP_H264_LOGI(TAG, "Reusing cached %s workspace: %u/%u bytes",
                      name, required, *actual_size);
        return workspace;
    }
    if (*cached) {
        esp_h264_free(*cached);
        *cached = NULL;
        *cached_capacity = 0;
    }
    return NULL;
}

static void cache_largest_workspace(uint8_t *workspace,
                                    uint32_t capacity,
                                    uint8_t **cached,
                                    uint32_t *cached_capacity,
                                    const char *name)
{
    if (!workspace) {
        return;
    }
    if (!*cached || capacity > *cached_capacity) {
        if (*cached) {
            esp_h264_free(*cached);
        }
        *cached = workspace;
        *cached_capacity = capacity;
        ESP_H264_LOGI(TAG, "Cached %s workspace: %u bytes", name, capacity);
        return;
    }
    esp_h264_free(workspace);
}

#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300]=])
string(FIND "${si_h264_param_patched}" "${si_h264_workspace_helpers_upstream}"
       si_h264_workspace_helpers_index)
if(si_h264_workspace_helpers_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 workspace helper insertion point not found; review the override")
endif()
string(REPLACE "${si_h264_workspace_helpers_upstream}"
               "${si_h264_workspace_helpers_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_set_fps_upstream [=[
static esp_h264_err_t set_fps(esp_h264_enc_param_handle_t handle, uint8_t fps)
{
    esp_h264_enc_param_hw_handle_t param_base = __containerof(handle, esp_h264_enc_param_hw_t, base);
    esp_h264_param_t *param = __containerof(param_base, esp_h264_param_t, hw_base);
    param->fps = fps;
    esp_h264_mutex_lock(param->mutex, ESP_H264_MAX_DELAY);
    if (param->rc_hd) {
        esp_h264_enc_hw_rc_set_bt_fps(param->rc_hd, param->bitrate, param->fps);
    }
    esp_h264_enc_set_sps(param->nal_buf, param->nal_buf_len, param->height, param->width, param->fps);
    esp_h264_mutex_unlock(param->mutex);
    return ESP_H264_ERR_OK;
}]=])
set(si_h264_set_fps_fixed [=[
static esp_h264_err_t set_fps(esp_h264_enc_param_handle_t handle, uint8_t fps)
{
    esp_h264_enc_param_hw_handle_t param_base = __containerof(handle, esp_h264_enc_param_hw_t, base);
    esp_h264_param_t *param = __containerof(param_base, esp_h264_param_t, hw_base);
    uint8_t next_nal[SPS_PPS_BUF_SIZE];
    uint16_t next_nal_bit_len = 0;

    esp_h264_mutex_lock(param->mutex, ESP_H264_MAX_DELAY);
    esp_h264_err_t ret = build_parameter_nals(
        next_nal, sizeof(next_nal), param->height, param->width, fps,
        param->qp_init, &next_nal_bit_len);
    if (ret != ESP_H264_ERR_OK) {
        esp_h264_mutex_unlock(param->mutex);
        return ret;
    }
    memcpy(param->nal_buf, next_nal, next_nal_bit_len >> 3);
    param->nal_bit_len = next_nal_bit_len;
    param->fps = fps;
    if (param->rc_hd) {
        esp_h264_enc_hw_rc_set_bt_fps(param->rc_hd, param->bitrate, fps);
    }
    esp_h264_mutex_unlock(param->mutex);
    return ESP_H264_ERR_OK;
}]=])
string(FIND "${si_h264_param_patched}" "${si_h264_set_fps_upstream}"
       si_h264_set_fps_index)
if(si_h264_set_fps_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 FPS setter changed; review transactional SPS/PPS rebuild")
endif()
string(REPLACE "${si_h264_set_fps_upstream}"
               "${si_h264_set_fps_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_initial_nal_upstream [=[
    param->nal_bit_len = esp_h264_enc_set_sps(param->nal_buf, param->nal_buf_len, param->height, param->width, param->fps);
    param->nal_bit_len += esp_h264_enc_set_pps(param->nal_buf + (param->nal_bit_len >> 3), param->nal_buf_len - (param->nal_bit_len >> 3), param->qp_init, true);]=])
set(si_h264_initial_nal_fixed [=[
    ret = build_parameter_nals(param->nal_buf, param->nal_buf_len,
                               param->height, param->width, param->fps,
                               param->qp_init, &param->nal_bit_len);
    ESP_H264_GOTO_ON_FALSE(ret == ESP_H264_ERR_OK, ret, __exit__, TAG,
                           "Invalid SPS/PPS length");]=])
string(FIND "${si_h264_param_patched}" "${si_h264_initial_nal_upstream}"
       si_h264_initial_nal_index)
if(si_h264_initial_nal_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 initial SPS/PPS build changed; review the NAL length gate")
endif()
string(REPLACE "${si_h264_initial_nal_upstream}"
               "${si_h264_initial_nal_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_reference_reserve_upstream [=[
static int max_refame_buffer_size(int16_t mb_width)
{
    /** H264_DMA_MACRO_SIZE + H264_DMA_HALF_MACRO_SIZE : Y(16) + U(4) + V(4) */
    return H264_DMA_3_LINES * H264_DMA_MACRO_SIZE * (H264_DMA_MACRO_SIZE + H264_DMA_HALF_MACRO_SIZE) * mb_width + HW_DMA_BUF_ALIGN_SIZE;
}]=])
set(si_h264_reference_reserve_fixed [=[
static int max_refame_buffer_size(int16_t mb_width)
{
    /** H264_DMA_MACRO_SIZE + H264_DMA_HALF_MACRO_SIZE : Y(16) + U(4) + V(4) */
    return H264_DMA_3_LINES * H264_DMA_MACRO_SIZE * (H264_DMA_MACRO_SIZE + H264_DMA_HALF_MACRO_SIZE) * mb_width + HW_DMA_BUF_ALIGN_SIZE;
}

_Static_assert(H264_DMA_3_LINES * H264_DMA_MACRO_SIZE *
                   (H264_DMA_MACRO_SIZE + H264_DMA_HALF_MACRO_SIZE) *
                   ((1920 + 15) >> 4) + HW_DMA_BUF_ALIGN_SIZE == 138247,
               "ESP32-P4 1920-wide H.264 reference workspace changed");

/* ExoAnchor boot-time reservation hook.  It is called once before any H.264
 * stream task exists; subsequent cache ownership is serialized by the
 * application's single-codec gate.  The encoder parameter allocator below
 * consumes this exact cache through take_cached_workspace(). */
esp_h264_err_t esp_h264_exoanchor_reserve_ref_workspace(
    uint16_t width, uint32_t *required_bytes, uint32_t *capacity_bytes)
{
    if (required_bytes) {
        *required_bytes = 0;
    }
    if (capacity_bytes) {
        *capacity_bytes = 0;
    }
    if (width < ESP_H264_MIN_WIDTH || width > ESP_H264_MAX_WIDTH) {
        return ESP_H264_ERR_ARG;
    }

    uint16_t mb_width = (width + 15U) >> 4;
    uint32_t required = (uint32_t)max_refame_buffer_size((int16_t)mb_width);
    if (required_bytes) {
        *required_bytes = required;
    }
    if (s_cached_ref_workspace && s_cached_ref_capacity >= required &&
        esp_ptr_internal(s_cached_ref_workspace)) {
        if (capacity_bytes) {
            *capacity_bytes = s_cached_ref_capacity;
        }
        ESP_H264_LOGI(TAG,
                      "ExoAnchor internal reference workspace already reserved: %u/%u bytes",
                      required, s_cached_ref_capacity);
        return ESP_H264_ERR_OK;
    }

    uint32_t actual_size = 0;
    /* esp_h264_aligned_malloc() asks esp_cache_get_alignment(INTERNAL),
     * which is the P4 L1 data-cache line, not the external-memory L2 line. */
    uint32_t minimum_capacity =
        ALIGN_UP(required, CONFIG_CACHE_L1_CACHE_LINE_SIZE);
    uint8_t *reserved = (uint8_t *)esp_h264_aligned_malloc(
        16, 1, required, &actual_size, ESP_H264_MEM_INTERNAL);
    bool reserved_internal = reserved && esp_ptr_internal(reserved);
    if (!reserved || actual_size < minimum_capacity || !reserved_internal) {
        ESP_H264_LOGE(TAG,
                      "ExoAnchor reference reserve rejected: required=%u actual=%u minimum=%u ptr=%p internal=%d",
                      required, actual_size, minimum_capacity, reserved,
                      reserved_internal);
        if (reserved) {
            esp_h264_free(reserved);
        }
        return ESP_H264_ERR_MEM;
    }

    if (s_cached_ref_workspace) {
        esp_h264_free(s_cached_ref_workspace);
    }
    s_cached_ref_workspace = reserved;
    s_cached_ref_capacity = actual_size;
    if (capacity_bytes) {
        *capacity_bytes = actual_size;
    }
    ESP_H264_LOGI(TAG,
                  "ExoAnchor internal reference workspace reserved: %u/%u bytes",
                  required, actual_size);
    return ESP_H264_ERR_OK;
}

void esp_h264_exoanchor_release_cached_workspaces(void)
{
    if (s_cached_ref_workspace) {
        esp_h264_free(s_cached_ref_workspace);
        s_cached_ref_workspace = NULL;
        s_cached_ref_capacity = 0;
    }
    if (s_cached_db_workspace) {
        esp_h264_free(s_cached_db_workspace);
        s_cached_db_workspace = NULL;
        s_cached_db_capacity = 0;
    }
    ESP_H264_LOGI(TAG, "ExoAnchor cached codec workspaces released");
}]=])
string(FIND "${si_h264_param_patched}"
       "${si_h264_reference_reserve_upstream}"
       si_h264_reference_reserve_index)
if(si_h264_reference_reserve_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 reference reserve insertion point not found; review the override")
endif()
string(REPLACE "${si_h264_reference_reserve_upstream}"
               "${si_h264_reference_reserve_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_workspace_free_upstream [=[
        if (param->ref) {
            esp_h264_free(param->ref);
        }
        if (param->db) {
            esp_h264_free(param->db);
        }]=])
set(si_h264_workspace_free_fixed [=[
        if (param->ref) {
            if (esp_ptr_internal(param->ref)) {
                cache_largest_workspace(param->ref, param->ref_capacity,
                                        &s_cached_ref_workspace,
                                        &s_cached_ref_capacity, "reference");
            } else {
                ESP_H264_LOGE(TAG,
                              "Rejecting non-internal reference workspace from cache");
                esp_h264_free(param->ref);
            }
            param->ref = NULL;
        }
        if (param->db) {
            cache_largest_workspace(param->db, param->db_capacity,
                                    &s_cached_db_workspace,
                                    &s_cached_db_capacity, "deblocking");
            param->db = NULL;
        }]=])
string(FIND "${si_h264_param_patched}" "${si_h264_workspace_free_upstream}"
       si_h264_workspace_free_index)
if(si_h264_workspace_free_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 workspace release fragment not found; review the override")
endif()
string(REPLACE "${si_h264_workspace_free_upstream}"
               "${si_h264_workspace_free_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_workspace_alloc_upstream [=[
    /** Allocated reference frame and DB memory (malloc: large buffers, no need to zero) */
    param->ref = (uint8_t *)esp_h264_aligned_malloc(16, 1, max_refame_buffer_size(param->mb_width), &actual_size, ESP_H264_MEM_INTERNAL);
    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, "No memory for reference frame");
    param->db = (uint8_t *)esp_h264_malloc_prefer(1, max_db_buffer_size(param->mb_width, param->mb_height), &actual_size, ESP_H264_MEM_INTERNAL, ESP_H264_MEM_SPIRAM);
    ESP_H264_GOTO_ON_FALSE(param->db, ESP_H264_ERR_MEM, __exit__, TAG, "No memory for data");]=])
set(si_h264_workspace_alloc_fixed [=[
    /** Allocated reference frame and DB memory (malloc: large buffers, no need to zero) */
    uint32_t ref_required = max_refame_buffer_size(param->mb_width);
    actual_size = 0;
    param->ref = take_cached_workspace(&s_cached_ref_workspace,
                                       &s_cached_ref_capacity,
                                       ref_required, &actual_size,
                                       "reference");
    if (!param->ref) {
        param->ref = (uint8_t *)esp_h264_aligned_malloc(
            16, 1, ref_required, &actual_size, ESP_H264_MEM_INTERNAL);
    }
    if (param->ref && !esp_ptr_internal(param->ref)) {
        ESP_H264_LOGE(TAG,
                      "Rejecting non-internal reference workspace allocation");
        esp_h264_free(param->ref);
        param->ref = NULL;
        actual_size = 0;
    }
    param->ref_capacity = actual_size;
    ESP_H264_GOTO_ON_FALSE(param->ref, ESP_H264_ERR_MEM, __exit__, TAG, "No memory for reference frame");

    uint32_t db_required = max_db_buffer_size(param->mb_width,
                                              param->mb_height);
    actual_size = 0;
    param->db = take_cached_workspace(&s_cached_db_workspace,
                                      &s_cached_db_capacity,
                                      db_required, &actual_size,
                                      "deblocking");
    if (!param->db) {
        param->db = (uint8_t *)esp_h264_malloc_prefer(1, db_required,
                                                      &actual_size,
                                                      ESP_H264_MEM_INTERNAL,
                                                      ESP_H264_MEM_SPIRAM);
    }
    param->db_capacity = actual_size;
    ESP_H264_GOTO_ON_FALSE(param->db, ESP_H264_ERR_MEM, __exit__, TAG, "No memory for data");]=])
string(FIND "${si_h264_param_patched}" "${si_h264_workspace_alloc_upstream}"
       si_h264_workspace_alloc_index)
if(si_h264_workspace_alloc_index EQUAL -1)
    message(FATAL_ERROR
        "esp_h264 workspace allocation fragment not found; review the override")
endif()
string(REPLACE "${si_h264_workspace_alloc_upstream}"
               "${si_h264_workspace_alloc_fixed}"
               si_h264_param_patched "${si_h264_param_patched}")

set(si_h264_patch_dir "${CMAKE_BINARY_DIR}/exoanchor-patched/esp_h264")
file(MAKE_DIRECTORY "${si_h264_patch_dir}")
set(si_h264_patched_single
    "${si_h264_patch_dir}/esp_h264_enc_single_hw.c")
set(si_h264_patched_param
    "${si_h264_patch_dir}/esp_h264_enc_hw_param.c")
set(si_h264_patched_nal
    "${si_h264_patch_dir}/h264_nal.c")
set(si_h264_patched_cache
    "${si_h264_patch_dir}/esp_h264_cache.c")
file(WRITE "${si_h264_patched_single}" "${si_h264_single_patched}")
file(WRITE "${si_h264_patched_param}" "${si_h264_param_patched}")
file(WRITE "${si_h264_patched_nal}" "${si_h264_nal_patched}")
file(WRITE "${si_h264_patched_cache}" "${si_h264_cache_guard_fixed}")
get_target_property(si_h264_sources ${si_h264_component} SOURCES)
list(FIND si_h264_sources "${si_h264_upstream_single}"
     si_h264_single_source_index)
if(si_h264_single_source_index EQUAL -1)
    message(FATAL_ERROR "Could not replace esp_h264 single-HW encoder source")
endif()
list(FIND si_h264_sources "${si_h264_upstream_param}"
     si_h264_param_source_index)
if(si_h264_param_source_index EQUAL -1)
    message(FATAL_ERROR "Could not replace esp_h264 HW parameter source")
endif()
list(FIND si_h264_sources "${si_h264_upstream_nal}"
     si_h264_nal_source_index)
if(si_h264_nal_source_index EQUAL -1)
    message(FATAL_ERROR "Could not replace esp_h264 H.264 NAL source")
endif()
list(FIND si_h264_sources "${si_h264_upstream_cache}"
     si_h264_cache_source_index)
if(si_h264_cache_source_index EQUAL -1)
    message(FATAL_ERROR "Could not replace esp_h264 cache wrapper source")
endif()
list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_single}")
list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_param}")
list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_nal}")
list(REMOVE_ITEM si_h264_sources "${si_h264_upstream_cache}")
list(APPEND si_h264_sources
     "${si_h264_patched_single}"
     "${si_h264_patched_param}"
     "${si_h264_patched_nal}"
     "${si_h264_patched_cache}")
set_property(TARGET ${si_h264_component} PROPERTY SOURCES
             "${si_h264_sources}")
target_include_directories(${si_h264_component} PRIVATE
    "${si_h264_component_dir}/hw/src")
message(STATUS "Using ExoAnchor esp_h264 DMA, cache-range, workspace, and SPS escaping overrides")
endif()

# libssh2 through 1.11.1 is affected by CVE-2026-55200: packet_length can
# overflow total_num before the later aggregate bound check. The ESP component
# has not yet published a fixed release, so reproduce the upstream one-hunk
# patch in the build directory. Guard the complete input hash and exact source
# fragment so a component update cannot silently inherit or misapply it.
idf_component_get_property(si_libssh2_component skuodi__libssh2_esp COMPONENT_LIB)
idf_component_get_property(si_libssh2_component_dir skuodi__libssh2_esp COMPONENT_DIR)
set(si_libssh2_transport
    "${si_libssh2_component_dir}/libssh2/src/transport.c")
set(si_libssh2_transport_sha256
    "adcbdc912a3de76fc87a9018feae1ebac2bfe3705b34ddedb3ff47eea144fbc9")
file(SHA256 "${si_libssh2_transport}" si_libssh2_transport_actual_sha256)
if(NOT si_libssh2_transport_actual_sha256 STREQUAL si_libssh2_transport_sha256)
    message(FATAL_ERROR
        "libssh2 transport.c changed; remove or re-review the CVE-2026-55200 backport")
endif()

file(READ "${si_libssh2_transport}" si_libssh2_transport_source)
set(si_libssh2_vulnerable_fragment
"                p->packet_length = _libssh2_ntohu32(block);
                if(p->packet_length < 1)
                    return LIBSSH2_ERROR_DECRYPT;")
set(si_libssh2_fixed_fragment
"                p->packet_length = _libssh2_ntohu32(block);
                if(p->packet_length < 1) {
                    return LIBSSH2_ERROR_DECRYPT;
                }
                else if(p->packet_length > LIBSSH2_PACKET_MAXPAYLOAD) {
                    return LIBSSH2_ERROR_OUT_OF_BOUNDARY;
                }")
string(FIND "${si_libssh2_transport_source}"
       "${si_libssh2_vulnerable_fragment}" si_libssh2_fragment_index)
if(si_libssh2_fragment_index EQUAL -1)
    message(FATAL_ERROR
        "libssh2 vulnerable transport fragment not found; review the security backport")
endif()
string(REPLACE "${si_libssh2_vulnerable_fragment}"
               "${si_libssh2_fixed_fragment}"
               si_libssh2_transport_patched
               "${si_libssh2_transport_source}")
set(si_libssh2_patch_dir "${CMAKE_BINARY_DIR}/exoanchor-patched/libssh2")
file(MAKE_DIRECTORY "${si_libssh2_patch_dir}")
set(si_libssh2_patched_transport "${si_libssh2_patch_dir}/transport.c")
file(WRITE "${si_libssh2_patched_transport}"
     "${si_libssh2_transport_patched}")

get_target_property(si_libssh2_sources ${si_libssh2_component} SOURCES)
list(FIND si_libssh2_sources "${si_libssh2_transport}"
     si_libssh2_transport_index)
if(si_libssh2_transport_index EQUAL -1)
    message(FATAL_ERROR "Could not replace libssh2 transport source")
endif()
list(REMOVE_ITEM si_libssh2_sources "${si_libssh2_transport}")
list(APPEND si_libssh2_sources "${si_libssh2_patched_transport}")
set_property(TARGET ${si_libssh2_component} PROPERTY SOURCES
             "${si_libssh2_sources}")
message(STATUS "Using upstream CVE-2026-55200 libssh2 transport fix")
