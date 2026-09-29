// Device status serialization and HTTP adapters.
#include "device_http.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agent_tools_settings.h"
#include "app_config.h"
#if SI_CFG_EMBEDDED_AGENT_ENABLED
#include "agent_repository.h"
#endif
#include "console_credentials.h"
#include "control_lease.h"
#include "device_observation_utils.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_json.h"
#include "http_api.h"
#include "ms2109_power.h"
#include "ms2109_test.h"
#include "storage_layout.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_pins.h"
#include "utf8_utils.h"
#if SI_CFG_VIDEO_H264_ENABLED
#include "video_h264_stream.h"
#endif
#include "web_json.h"

#define HID_ACTIONS_MAX_BODY 4096
#define HID_ACTIONS_MAX_ITEMS 64
#define HID_ACTIONS_WAIT_SLICE_MS 100U

extern bool si_web_ssh_target_available(void);

static bool embedded_agent_storage_ready(void)
{
#if SI_CFG_EMBEDDED_AGENT_ENABLED
    return agent_history_tf_mounted();
#else
    return false;
#endif
}

static void *device_http_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : calloc(count, size);
}

static void device_http_add_utf8_string(cJSON *object, const char *name,
                                        const char *value, size_t max_len)
{
    char *safe = si_utf8_sanitize(value ? value : "", max_len);
    cJSON_AddStringToObject(object, name, safe ? safe : "");
    free(safe);
}

static const char *device_json_string_any(const cJSON *obj, const char *first,
                                          const char *second, const char *third)
{
    const char *names[] = {first, second, third};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i]) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, names[i]);
        if (cJSON_IsString(item)) {
            return item->valuestring;
        }
    }
    return NULL;
}

static void add_flash_storage_json(cJSON *root, const char *name,
                                   const si_device_flash_observation_t *info)
{
    cJSON *flash = cJSON_AddObjectToObject(root, name);
    cJSON_AddStringToObject(flash, "type", "NOR");
    cJSON_AddStringToObject(flash, "usage_basis", "partition_map_allocated");
    cJSON_AddBoolToObject(flash, "detected", info->detected);
    cJSON_AddNumberToObject(flash, "total_bytes", info->total_bytes);
    cJSON_AddNumberToObject(flash, "used_bytes", info->partition_bytes);
    cJSON_AddNumberToObject(flash, "free_bytes", info->free_bytes);
    cJSON_AddNumberToObject(flash, "allocated_bytes", info->partition_bytes);
    cJSON_AddNumberToObject(flash, "unallocated_bytes", info->free_bytes);
    cJSON_AddNumberToObject(flash, "partition_bytes", info->partition_bytes);
    cJSON_AddNumberToObject(flash, "layout_end_bytes", info->reserved_bytes);
    cJSON_AddNumberToObject(flash, "layout_gap_bytes", info->layout_gap_bytes);
    cJSON_AddNumberToObject(flash, "app_partition_bytes", info->app_partition_bytes);
    cJSON_AddNumberToObject(flash, "data_partition_bytes", info->data_partition_bytes);
    cJSON_AddNumberToObject(flash, "partition_count", info->partition_count);
    cJSON_AddNumberToObject(flash, "usage_percent",
                            si_observation_used_percent(info->total_bytes,
                                                        info->free_bytes));
    cJSON_AddStringToObject(flash, "running_partition", info->running_partition);
    cJSON_AddStringToObject(flash, "boot_partition", info->boot_partition);
    cJSON_AddStringToObject(flash, "next_update_partition", info->next_update_partition);
    cJSON_AddNumberToObject(flash, "running_partition_bytes",
                            info->running_partition_bytes);
    cJSON_AddNumberToObject(flash, "next_update_partition_bytes",
                            info->next_update_partition_bytes);

    cJSON *nvs = cJSON_AddObjectToObject(flash, "nvs");
    cJSON_AddBoolToObject(nvs, "stats_available", info->nvs_stats_available);
    cJSON_AddNumberToObject(nvs, "partition_bytes", info->nvs_partition_bytes);
    cJSON_AddNumberToObject(nvs, "total_entries", info->nvs_total_entries);
    cJSON_AddNumberToObject(nvs, "used_entries", info->nvs_used_entries);
    cJSON_AddNumberToObject(nvs, "free_entries", info->nvs_free_entries);
    cJSON_AddNumberToObject(nvs, "namespace_count", info->nvs_namespace_count);
    cJSON_AddNumberToObject(nvs, "usage_percent",
                            si_observation_used_percent(info->nvs_total_entries,
                                                        info->nvs_free_entries));
}

static void add_tf_storage_json(cJSON *root, const char *name,
                                const si_device_tf_observation_t *info)
{
    cJSON *tf = cJSON_AddObjectToObject(root, name);
    cJSON_AddStringToObject(tf, "type", "TF");
    cJSON_AddBoolToObject(tf, "supported", info->supported);
    cJSON_AddBoolToObject(tf, "mounted", info->mounted);
    cJSON_AddBoolToObject(tf, "detected", info->mounted);
    cJSON_AddBoolToObject(tf, "degraded_mode", info->degraded_mode);
    cJSON_AddStringToObject(tf, "mount_point", info->mount_point);
    cJSON_AddStringToObject(tf, "card_name", info->card_name);
    cJSON_AddStringToObject(tf, "card_type", info->card_type);
    cJSON_AddNumberToObject(tf, "total_bytes", (double)info->total_bytes);
    cJSON_AddNumberToObject(tf, "used_bytes", (double)info->used_bytes);
    cJSON_AddNumberToObject(tf, "free_bytes", (double)info->free_bytes);
    cJSON_AddNumberToObject(tf, "usage_percent", info->usage_percent);
    cJSON_AddNumberToObject(tf, "sector_size", info->sector_size);
    cJSON_AddNumberToObject(tf, "cluster_size", info->cluster_size);
    cJSON_AddNumberToObject(tf, "bus_frequency_khz", info->bus_frequency_khz);
    cJSON_AddNumberToObject(tf, "bus_width", info->bus_width);
    cJSON_AddStringToObject(tf, "bus_mode", info->bus_mode);
    cJSON_AddStringToObject(tf, "fallback_reason", info->fallback_reason);
    cJSON_AddStringToObject(tf, "last_error", info->last_error);
    cJSON_AddStringToObject(tf, "root_path", SI_STORAGE_ROOT);
    cJSON_AddStringToObject(tf, "assets_path", SI_STORAGE_ASSETS_DIR);
    cJSON_AddStringToObject(tf, "agent_path", SI_STORAGE_AGENT_DIR);
    cJSON_AddStringToObject(tf, "logs_path", SI_STORAGE_LOGS_DIR);
    cJSON_AddStringToObject(tf, "snapshots_path", SI_STORAGE_SNAPSHOTS_DIR);
}

static void add_storage_device_summary(cJSON *devices, const char *id,
                                       const char *label, const char *kind,
                                       const char *bus, const char *role,
                                       bool supported, bool detected, bool mounted,
                                       uint64_t total_bytes, uint64_t used_bytes,
                                       uint64_t free_bytes, double usage_percent,
                                       const char *last_error)
{
    if (!devices) {
        return;
    }
    cJSON *device = cJSON_CreateObject();
    if (!device) {
        return;
    }
    cJSON_AddItemToArray(devices, device);
    cJSON_AddStringToObject(device, "id", id);
    cJSON_AddStringToObject(device, "label", label);
    cJSON_AddStringToObject(device, "kind", kind);
    cJSON_AddStringToObject(device, "bus", bus);
    cJSON_AddStringToObject(device, "role", role);
    cJSON_AddBoolToObject(device, "supported", supported);
    cJSON_AddBoolToObject(device, "detected", detected);
    cJSON_AddBoolToObject(device, "mounted", mounted);
    cJSON_AddNumberToObject(device, "total_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(device, "used_bytes", (double)used_bytes);
    cJSON_AddNumberToObject(device, "free_bytes", (double)free_bytes);
    cJSON_AddNumberToObject(device, "usage_percent", usage_percent);
    cJSON_AddStringToObject(device, "last_error", last_error ? last_error : "");
}

static void add_storage_expansion_slot(cJSON *slots, const char *id,
                                       const char *label, const char *bus,
                                       const char *role)
{
    if (!slots) {
        return;
    }
    cJSON *slot = cJSON_CreateObject();
    if (!slot) {
        return;
    }
    cJSON_AddItemToArray(slots, slot);
    cJSON_AddStringToObject(slot, "id", id);
    cJSON_AddStringToObject(slot, "label", label);
    cJSON_AddStringToObject(slot, "bus", bus);
    cJSON_AddStringToObject(slot, "role", role);
    cJSON_AddBoolToObject(slot, "supported", false);
    cJSON_AddBoolToObject(slot, "implemented", false);
}

esp_err_t system_info_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_system_observation_t status;
    si_device_observation_get_system(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddNumberToObject(cpu, "usage_percent", 0);
    cJSON_AddNumberToObject(cpu, "cores", status.cpu_cores);
    cJSON_AddNumberToObject(cpu, "freq_mhz", 0);

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    cJSON_AddNumberToObject(memory, "total_mb", 0);
    cJSON_AddNumberToObject(memory, "used_mb", 0);
    cJSON_AddNumberToObject(memory, "available_mb", status.free_heap_bytes / 1024 / 1024);
    cJSON_AddNumberToObject(memory, "free_bytes", status.free_heap_bytes);
    cJSON_AddNumberToObject(memory, "minimum_free_bytes", status.minimum_free_heap_bytes);
    cJSON_AddNumberToObject(memory, "usage_percent", 0);

    cJSON *temp = cJSON_AddObjectToObject(root, "temperature");
    cJSON_AddNumberToObject(temp, "celsius", 0);
    cJSON_AddStringToObject(temp, "source", "unavailable");

    cJSON *disk = cJSON_AddObjectToObject(root, "disk");
    cJSON_AddNumberToObject(disk, "total_gb", status.flash.total_bytes / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "used_gb", status.flash.partition_bytes / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "free_gb", status.flash.free_bytes / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "usage_percent",
                            si_observation_used_percent(status.flash.total_bytes,
                                                        status.flash.free_bytes));
    add_flash_storage_json(disk, "flash", &status.flash);
    add_tf_storage_json(disk, "tf_card", &status.tf);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON *eth = cJSON_AddObjectToObject(network, "ethernet");
    cJSON_AddBoolToObject(eth, "up", status.network.connected);
    cJSON_AddBoolToObject(eth, "link_up", status.network.link_up);
    cJSON_AddNumberToObject(eth, "speed_mbps", status.network.speed_mbps);
    cJSON_AddBoolToObject(eth, "full_duplex", status.network.full_duplex);
    cJSON_AddStringToObject(eth, "ipv4", status.network.ip[0] ? status.network.ip : "No IP");
    cJSON_AddStringToObject(eth, "netmask", status.network.netmask);
    cJSON_AddStringToObject(eth, "gateway", status.network.gateway);
    cJSON_AddStringToObject(eth, "mac", status.network.mac);

    cJSON *uptime_obj = cJSON_AddObjectToObject(root, "uptime");
    cJSON_AddNumberToObject(uptime_obj, "seconds", status.uptime_seconds);
    cJSON_AddStringToObject(uptime_obj, "formatted", status.uptime_formatted);

    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    cJSON_AddNumberToObject(auth, "login_count", status.login_count);

    cJSON *load = cJSON_AddObjectToObject(root, "load");
    cJSON_AddNumberToObject(load, "1min", 0);
    cJSON_AddNumberToObject(load, "5min", 0);
    cJSON_AddNumberToObject(load, "15min", 0);
    cJSON_AddStringToObject(root, "device_label", status.device_label);
    cJSON_AddStringToObject(root, "label", status.device_label);
    cJSON_AddStringToObject(root, "hostname", status.hostname);
    cJSON_AddStringToObject(root, "build_profile", SI_BUILD_PROFILE);
    cJSON_AddBoolToObject(root, "embedded_agent",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);

    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t logs_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    int count = 50;
    char query[64] = {0};
    char nbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "n", nbuf, sizeof(nbuf)) == ESP_OK) {
        count = atoi(nbuf);
        if (count < 1) {
            count = 1;
        } else if (count > SI_DEVICE_OBSERVATION_MAX_LOGS) {
            count = SI_DEVICE_OBSERVATION_MAX_LOGS;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *logs = cJSON_AddArrayToObject(root, "logs");
    si_device_log_observation_t *snapshot =
        device_http_calloc((size_t)count, sizeof(*snapshot));
    size_t total = snapshot ?
                   si_device_observation_log_snapshot(snapshot, (size_t)count) : 0;
    for (size_t i = 0; i < total; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "time", snapshot[i].time);
        cJSON_AddStringToObject(entry, "level", snapshot[i].level);
        device_http_add_utf8_string(
            entry, "message", snapshot[i].message,
            sizeof(snapshot[i].message) - 1U);
        cJSON_AddItemToArray(logs, entry);
    }
    free(snapshot);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t logs_download_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    int count = SI_DEVICE_OBSERVATION_MAX_LOGS;
    char query[64] = {0};
    char nbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "n", nbuf, sizeof(nbuf)) == ESP_OK) {
        count = atoi(nbuf);
        if (count < 1) {
            count = 1;
        } else if (count > SI_DEVICE_OBSERVATION_MAX_LOGS) {
            count = SI_DEVICE_OBSERVATION_MAX_LOGS;
        }
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"exoanchor-system-logs.txt\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char line[256] = {0};
    int n = snprintf(line, sizeof(line),
                     "ExoAnchor ESP32-P4 logs\nversion=%s\ncount=%d\n\n",
                     SI_BMC_VERSION, count);
    if (n > 0 && httpd_resp_send_chunk(req, line, (size_t)n) != ESP_OK) {
        return ESP_FAIL;
    }

    si_device_log_observation_t *snapshot =
        device_http_calloc((size_t)count, sizeof(*snapshot));
    size_t total = snapshot ?
                   si_device_observation_log_snapshot(snapshot, (size_t)count) : 0;
    for (size_t i = 0; i < total; i++) {
        char *safe_message = si_utf8_sanitize(
            snapshot[i].message, sizeof(snapshot[i].message) - 1U);
        n = snprintf(line, sizeof(line), "%s %-5s %s\n",
                     snapshot[i].time, snapshot[i].level,
                     safe_message ? safe_message : "");
        free(safe_message);
        if (n < 0) {
            continue;
        }
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        if (httpd_resp_send_chunk(req, line, len) != ESP_OK) {
            free(snapshot);
            return ESP_FAIL;
        }
    }
    free(snapshot);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void add_video_control_json(
    cJSON *root, const si_device_video_control_observation_t *status)
{
    cJSON *control = cJSON_AddObjectToObject(root, "control");
    cJSON_AddBoolToObject(control, "settings_stored_in_nvs", true);
    cJSON_AddBoolToObject(control, "preview_enabled", status->preview_enabled);
    cJSON_AddBoolToObject(control, "always_on", status->always_on);
    cJSON_AddBoolToObject(control, "always_on_stored_in_nvs", true);
    cJSON_AddBoolToObject(control, "kvm_active", status->kvm_active);
    cJSON_AddBoolToObject(control, "kvm_blocked_by_agent", status->agent_takeover);
    cJSON_AddNumberToObject(control, "kvm_grace_ms", status->kvm_grace_ms);
    cJSON_AddNumberToObject(control, "kvm_grace_remaining_ms", status->kvm_remaining_ms);
    cJSON_AddBoolToObject(control, "agent_active", status->agent_active);
    cJSON_AddBoolToObject(control, "agent_takeover", status->agent_takeover);
    cJSON_AddNumberToObject(control, "agent_grace_ms", status->agent_grace_ms);
    cJSON_AddNumberToObject(control, "agent_grace_remaining_ms", status->agent_remaining_ms);
    cJSON_AddBoolToObject(control, "active_enabled", status->active_enabled);
    cJSON_AddStringToObject(control, "active_owner", status->active_owner);

    cJSON *preview = cJSON_AddObjectToObject(control, "preview_mode");
    cJSON_AddBoolToObject(preview, "stored_in_nvs", true);
    cJSON_AddNumberToObject(preview, "width", status->preview_width);
    cJSON_AddNumberToObject(preview, "height", status->preview_height);
    cJSON_AddNumberToObject(preview, "fps", status->preview_fps_x100 / 100.0);
    cJSON_AddNumberToObject(preview, "fps_x100", status->preview_fps_x100);
    cJSON_AddNumberToObject(preview, "stride_ms", status->preview_stride_ms);

    cJSON *kvm = cJSON_AddObjectToObject(control, "kvm_mode");
    cJSON_AddBoolToObject(kvm, "stored_in_nvs", true);
    cJSON_AddNumberToObject(kvm, "width", status->kvm_width);
    cJSON_AddNumberToObject(kvm, "height", status->kvm_height);
    cJSON_AddNumberToObject(kvm, "fps", status->kvm_fps_x100 / 100.0);
    cJSON_AddNumberToObject(kvm, "fps_x100", status->kvm_fps_x100);

    cJSON *active = cJSON_AddObjectToObject(control, "active_mode");
    cJSON_AddNumberToObject(active, "width", status->active_width);
    cJSON_AddNumberToObject(active, "height", status->active_height);
    cJSON_AddNumberToObject(active, "fps", status->active_fps_x100 / 100.0);
    cJSON_AddNumberToObject(active, "fps_x100", status->active_fps_x100);
    cJSON_AddNumberToObject(active, "stride_ms", status->active_stride_ms);
}

void si_device_http_add_video_json(cJSON *root,
                                   const si_device_video_observation_t *video,
                                   const si_device_video_control_observation_t *control,
                                   const si_device_video_mode_observation_t *modes,
                                   size_t mode_count)
{
    bool device_connected = video->modes_count > 0;
    const char *state =
        !video->enabled ? "disabled" :
        video->frame_ready ? "active" :
        device_connected ? "standby" :
        video->initialized ? "waiting" : "offline";
    cJSON_AddBoolToObject(root, "enabled", video->enabled);
    // Keep the legacy `connected` field aligned with physical UVC enumeration.
    // Consumers that require a readable image must use `frame_ready`.
    cJSON_AddBoolToObject(root, "connected", device_connected);
    cJSON_AddBoolToObject(root, "device_connected", device_connected);
    cJSON_AddBoolToObject(root, "frame_ready", video->frame_ready);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddBoolToObject(root, "initialized", video->initialized);
    cJSON_AddBoolToObject(root, "streaming", video->streaming);
    cJSON_AddBoolToObject(root, "capture_enabled", video->capture_enabled);
    cJSON_AddBoolToObject(root, "always_online", control->always_on);
    cJSON_AddNumberToObject(root, "capture_stride_ms", video->capture_stride_ms);
    cJSON_AddStringToObject(root, "capture_owner", video->capture_owner);
    cJSON_AddStringToObject(root, "source", video->source);
    cJSON_AddStringToObject(root, "pixel_format", video->pixel_format);
    cJSON_AddNumberToObject(root, "width", video->width);
    cJSON_AddNumberToObject(root, "height", video->height);
    char resolution[24];
    snprintf(resolution, sizeof(resolution), "%" PRIu32 "x%" PRIu32,
             video->width, video->height);
    cJSON_AddStringToObject(root, "resolution", resolution);
    cJSON_AddNumberToObject(root, "target_width", video->target_width);
    cJSON_AddNumberToObject(root, "target_height", video->target_height);
    cJSON_AddNumberToObject(root, "target_fps", video->target_fps_x100 / 100.0);
    cJSON_AddNumberToObject(root, "target_fps_x100", video->target_fps_x100);
    char target_resolution[24];
    snprintf(target_resolution, sizeof(target_resolution), "%" PRIu32 "x%" PRIu32,
             video->target_width, video->target_height);
    cJSON_AddStringToObject(root, "target_resolution", target_resolution);
    cJSON_AddNumberToObject(root, "modes_count", video->modes_count);
    cJSON_AddNumberToObject(root, "data_lanes", video->data_lanes);
    cJSON_AddNumberToObject(root, "lane_bitrate_mbps", video->lane_bitrate_mbps);
    cJSON_AddNumberToObject(root, "quality", video->jpeg_quality);
    cJSON_AddBoolToObject(root, "settings_stored_in_nvs", true);
    cJSON_AddNumberToObject(root, "frames_captured", video->frames_captured);
    cJSON_AddNumberToObject(root, "frames_encoded", video->frames_encoded);
    cJSON_AddNumberToObject(root, "frames_dropped", video->frames_dropped);
    cJSON_AddNumberToObject(root, "last_jpeg_size", video->last_jpeg_size);
    cJSON_AddNumberToObject(root, "frame_interval_ms",
                            video->frame_interval_ms);
    cJSON_AddNumberToObject(root, "last_frame_ms", video->last_frame_ms);
    cJSON_AddNumberToObject(root, "fps", video->fps_x100 / 100.0);
    cJSON *pipeline = cJSON_AddObjectToObject(root, "pipeline_stats");
    cJSON_AddNumberToObject(pipeline, "uvc_callbacks", video->uvc_callbacks);
    cJSON_AddNumberToObject(pipeline, "queue_drops", video->uvc_queue_drops);
    cJSON_AddNumberToObject(pipeline, "h264_pressure_coalesces",
                            video->h264_pressure_coalesces);
    cJSON_AddNumberToObject(pipeline, "buffer_underflows",
                            video->uvc_buffer_underflows);
    cJSON_AddNumberToObject(pipeline, "buffer_overflows",
                            video->uvc_buffer_overflows);
    cJSON_AddNumberToObject(pipeline, "return_pending",
                            video->uvc_return_pending);
    cJSON_AddNumberToObject(pipeline, "return_retries",
                            video->uvc_return_retries);
    cJSON_AddNumberToObject(pipeline, "return_failures",
                            video->uvc_return_failures);
    cJSON_AddNumberToObject(pipeline, "validate_last_us",
                            video->ingest_validate_last_us);
    cJSON_AddNumberToObject(pipeline, "validate_max_us",
                            video->ingest_validate_max_us);
    cJSON_AddNumberToObject(pipeline, "publish_last_us",
                            video->ingest_publish_last_us);
    cJSON_AddNumberToObject(pipeline, "publish_max_us",
                            video->ingest_publish_max_us);
    cJSON_AddNumberToObject(pipeline, "ingest_total_last_us",
                            video->ingest_total_last_us);
    cJSON_AddNumberToObject(pipeline, "ingest_total_max_us",
                            video->ingest_total_max_us);
    cJSON *stream_metrics = cJSON_AddObjectToObject(root, "stream_metrics");
    cJSON_AddBoolToObject(stream_metrics, "valid",
                          video->stream_metrics_valid);
    cJSON_AddNumberToObject(stream_metrics, "stream_id",
                            video->stream_metrics_stream_id);
    cJSON_AddNumberToObject(stream_metrics, "fps",
                            video->stream_fps_x100 / 100.0);
    cJSON_AddNumberToObject(stream_metrics, "fps_x100",
                            video->stream_fps_x100);
    cJSON_AddNumberToObject(stream_metrics, "bitrate_bps",
                            video->stream_bitrate_bps);
    cJSON_AddNumberToObject(stream_metrics, "bitrate_mbps",
                            video->stream_bitrate_bps / 1000000.0);
    cJSON_AddNumberToObject(stream_metrics, "sample_window_ms",
                            video->stream_sample_window_ms);
    cJSON_AddStringToObject(root, "last_error", video->last_error);
#if SI_CFG_VIDEO_H264_ENABLED
    si_h264_stream_status_t h264 = {0};
    si_h264_stream_get_status(&h264);
    cJSON *h264_runtime = cJSON_AddObjectToObject(root, "h264_runtime");
    cJSON_AddBoolToObject(h264_runtime, "compiled", true);
    cJSON_AddBoolToObject(h264_runtime, "service_initialized",
                          h264.service_initialized);
    cJSON_AddBoolToObject(h264_runtime, "resource_probe_attempted",
                          h264.resource_probe_attempted);
    cJSON_AddBoolToObject(h264_runtime, "available", h264.available);
    cJSON_AddBoolToObject(h264_runtime, "session_active",
                          h264.session_active);
    cJSON_AddBoolToObject(h264_runtime, "pipeline_ready",
                          h264.pipeline_ready);
    cJSON_AddBoolToObject(h264_runtime, "restore_pending",
                          h264.restore_pending);
    cJSON_AddBoolToObject(h264_runtime, "teardown_poisoned",
                          h264.teardown_poisoned);
    cJSON_AddStringToObject(h264_runtime, "resource_error",
                            esp_err_to_name(h264.resource_error));
    cJSON_AddStringToObject(h264_runtime, "restore_error",
                            esp_err_to_name(h264.restore_error));
    cJSON_AddNumberToObject(h264_runtime, "width", h264.width);
    cJSON_AddNumberToObject(h264_runtime, "height", h264.height);
    cJSON_AddNumberToObject(h264_runtime, "target_fps", h264.target_fps);
    cJSON *h264_resources =
        cJSON_AddObjectToObject(h264_runtime, "resources");
    cJSON_AddNumberToObject(h264_resources, "yuv_surfaces",
                            h264.yuv_surfaces);
    cJSON_AddNumberToObject(h264_resources, "au_slots", h264.au_slots);
    cJSON_AddNumberToObject(h264_resources, "primary_au_capacity_bytes",
                            h264.primary_au_capacity_bytes);
    cJSON_AddNumberToObject(h264_resources, "small_egress_capacity_bytes",
                            h264.small_egress_capacity_bytes);
    cJSON_AddBoolToObject(h264_resources,
                          "reference_workspace_reserve_attempted",
                          h264.reference_workspace_reserve_attempted);
    cJSON_AddBoolToObject(h264_resources, "reference_workspace_reserved",
                          h264.reference_workspace_reserved);
    cJSON_AddStringToObject(
        h264_resources, "reference_workspace_residency",
        h264.reference_workspace_reserved
            ? (h264.reference_workspace_internal ? "internal" : "psram")
            : "none");
    cJSON_AddNumberToObject(h264_resources,
                            "reference_workspace_required_bytes",
                            h264.reference_workspace_required_bytes);
    cJSON_AddNumberToObject(h264_resources,
                            "reference_workspace_capacity_bytes",
                            h264.reference_workspace_capacity_bytes);
    cJSON_AddStringToObject(h264_resources, "reference_workspace_error",
                            esp_err_to_name(
                                h264.reference_workspace_error));
    cJSON_AddBoolToObject(h264_resources, "overlap_enabled",
                          h264.overlap_enabled);
    cJSON_AddBoolToObject(h264_resources, "serial_fallback",
                          h264.serial_fallback);
    cJSON *h264_metrics =
        cJSON_AddObjectToObject(h264_runtime, "chain_metrics");
    cJSON_AddBoolToObject(h264_metrics, "valid", h264.metrics_valid);
    cJSON_AddNumberToObject(h264_metrics, "age_ms", h264.metrics_age_ms);
    cJSON_AddNumberToObject(h264_metrics, "sample_window_ms",
                            h264.metrics_window_ms);
    cJSON_AddNumberToObject(h264_metrics, "source_fps",
                            h264.source_fps_x100 / 100.0);
    cJSON_AddNumberToObject(h264_metrics, "output_fps",
                            h264.output_fps_x100 / 100.0);
    cJSON_AddNumberToObject(h264_metrics, "source_drops",
                            h264.source_drops);
    cJSON_AddNumberToObject(h264_metrics, "bitrate_bps",
                            h264.h264_bitrate_bps);
    cJSON_AddNumberToObject(h264_metrics, "decode_avg_us",
                            h264.decode_avg_us);
    cJSON_AddNumberToObject(h264_metrics, "encode_avg_us",
                            h264.encode_avg_us);
    cJSON_AddNumberToObject(h264_metrics, "send_avg_us",
                            h264.send_avg_us);
    cJSON_AddNumberToObject(h264_metrics, "decode_max_us",
                            h264.decode_max_us);
    cJSON_AddNumberToObject(h264_metrics, "encode_max_us",
                            h264.encode_max_us);
    cJSON_AddNumberToObject(h264_metrics, "send_max_us",
                            h264.send_max_us);
    cJSON_AddNumberToObject(h264_metrics, "overlap_pairs",
                            h264.overlap_pairs);
    cJSON_AddNumberToObject(h264_metrics, "overlap_misses",
                            h264.overlap_misses);
    cJSON_AddNumberToObject(h264_metrics, "overlap_wait_timeouts",
                            h264.overlap_wait_timeouts);
    cJSON_AddNumberToObject(h264_metrics, "backpressure_drops_total",
                            h264.backpressure_drops_total);
    cJSON_AddNumberToObject(h264_metrics, "decode_failures_total",
                            h264.decode_failures_total);
    cJSON_AddNumberToObject(h264_metrics, "encode_failures_total",
                            h264.encode_failures_total);
    cJSON_AddNumberToObject(h264_metrics, "send_failures_total",
                            h264.send_failures_total);
    cJSON_AddNumberToObject(h264_metrics, "sessions_started_total",
                            h264.sessions_started_total);
    cJSON_AddNumberToObject(h264_metrics, "sessions_failed_total",
                            h264.sessions_failed_total);
    cJSON_AddNumberToObject(h264_metrics, "pipeline_starts_total",
                            h264.pipeline_starts_total);
    cJSON_AddNumberToObject(h264_metrics, "access_units_total",
                            h264.access_units_total);
    cJSON_AddNumberToObject(h264_metrics, "last_sequence",
                            h264.last_sequence);
    cJSON_AddNumberToObject(h264_metrics, "last_timestamp_ms",
                            h264.last_timestamp_ms);
    cJSON_AddNumberToObject(h264_metrics, "last_jpeg_bytes",
                            h264.last_jpeg_bytes);
    cJSON_AddNumberToObject(h264_metrics, "max_jpeg_bytes",
                            h264.max_jpeg_bytes);
    cJSON_AddNumberToObject(h264_metrics, "last_au_bytes",
                            h264.last_au_bytes);
    cJSON_AddNumberToObject(h264_metrics, "max_au_bytes",
                            h264.max_au_bytes);
    cJSON_AddNumberToObject(h264_metrics,
                            "primary_egress_access_units_total",
                            h264.primary_egress_access_units_total);
    cJSON_AddNumberToObject(h264_metrics,
                            "small_egress_access_units_total",
                            h264.small_egress_access_units_total);
#else
    cJSON *h264_runtime = cJSON_AddObjectToObject(root, "h264_runtime");
    cJSON_AddBoolToObject(h264_runtime, "compiled", false);
    cJSON_AddBoolToObject(h264_runtime, "service_initialized", false);
    cJSON_AddBoolToObject(h264_runtime, "resource_probe_attempted", false);
    cJSON_AddBoolToObject(h264_runtime, "available", false);
    cJSON_AddBoolToObject(h264_runtime, "session_active", false);
    cJSON_AddBoolToObject(h264_runtime, "pipeline_ready", false);
    cJSON_AddBoolToObject(h264_runtime, "restore_pending", false);
    cJSON_AddBoolToObject(h264_runtime, "teardown_poisoned", false);
    cJSON_AddStringToObject(h264_runtime, "resource_error",
                            "ESP_ERR_NOT_SUPPORTED");
    cJSON_AddStringToObject(h264_runtime, "restore_error", "ESP_OK");
    cJSON_AddNumberToObject(h264_runtime, "width", 0);
    cJSON_AddNumberToObject(h264_runtime, "height", 0);
    cJSON_AddNumberToObject(h264_runtime, "target_fps", 0);
    cJSON *h264_resources =
        cJSON_AddObjectToObject(h264_runtime, "resources");
    cJSON_AddNumberToObject(h264_resources, "yuv_surfaces", 0);
    cJSON_AddNumberToObject(h264_resources, "au_slots", 0);
    cJSON_AddBoolToObject(h264_resources,
                          "reference_workspace_reserve_attempted", false);
    cJSON_AddBoolToObject(h264_resources, "reference_workspace_reserved",
                          false);
    cJSON_AddStringToObject(h264_resources,
                            "reference_workspace_residency", "none");
    cJSON_AddNumberToObject(h264_resources,
                            "reference_workspace_required_bytes", 0);
    cJSON_AddNumberToObject(h264_resources,
                            "reference_workspace_capacity_bytes", 0);
    cJSON_AddStringToObject(h264_resources, "reference_workspace_error",
                            "ESP_ERR_NOT_SUPPORTED");
    cJSON_AddBoolToObject(h264_resources, "overlap_enabled", false);
    cJSON_AddBoolToObject(h264_resources, "serial_fallback", false);
    cJSON *h264_metrics =
        cJSON_AddObjectToObject(h264_runtime, "chain_metrics");
    cJSON_AddBoolToObject(h264_metrics, "valid", false);
    cJSON_AddNumberToObject(h264_metrics, "age_ms", 0);
    cJSON_AddNumberToObject(h264_metrics, "sample_window_ms", 0);
    cJSON_AddNumberToObject(h264_metrics, "source_fps", 0);
    cJSON_AddNumberToObject(h264_metrics, "output_fps", 0);
    cJSON_AddNumberToObject(h264_metrics, "source_drops", 0);
    cJSON_AddNumberToObject(h264_metrics, "bitrate_bps", 0);
    cJSON_AddNumberToObject(h264_metrics, "decode_avg_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "encode_avg_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "send_avg_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "decode_max_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "encode_max_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "send_max_us", 0);
    cJSON_AddNumberToObject(h264_metrics, "overlap_pairs", 0);
    cJSON_AddNumberToObject(h264_metrics, "overlap_misses", 0);
    cJSON_AddNumberToObject(h264_metrics, "overlap_wait_timeouts", 0);
    cJSON_AddNumberToObject(h264_metrics, "backpressure_drops_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "decode_failures_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "encode_failures_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "send_failures_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "sessions_started_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "sessions_failed_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "pipeline_starts_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "access_units_total", 0);
    cJSON_AddNumberToObject(h264_metrics, "last_sequence", 0);
    cJSON_AddNumberToObject(h264_metrics, "last_timestamp_ms", 0);
    cJSON_AddNumberToObject(h264_metrics, "last_jpeg_bytes", 0);
    cJSON_AddNumberToObject(h264_metrics, "max_jpeg_bytes", 0);
    cJSON_AddNumberToObject(h264_metrics, "last_au_bytes", 0);
    cJSON_AddNumberToObject(h264_metrics, "max_au_bytes", 0);
#endif
    add_video_control_json(root, control);

    cJSON *modes_json = cJSON_AddArrayToObject(root, "modes");
    size_t copy_count = mode_count < SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES ?
                        mode_count : SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES;
    for (size_t i = 0; modes && i < copy_count; i++) {
        cJSON *mode = cJSON_CreateObject();
        cJSON_AddStringToObject(mode, "pixel_format", modes[i].pixel_format);
        cJSON_AddNumberToObject(mode, "width", modes[i].width);
        cJSON_AddNumberToObject(mode, "height", modes[i].height);
        char mode_resolution[24];
        snprintf(mode_resolution, sizeof(mode_resolution), "%" PRIu32 "x%" PRIu32,
                 modes[i].width, modes[i].height);
        cJSON_AddStringToObject(mode, "resolution", mode_resolution);
        cJSON_AddNumberToObject(mode, "fps", modes[i].fps_x100 / 100.0);
        cJSON_AddNumberToObject(mode, "fps_x100", modes[i].fps_x100);
        cJSON_AddBoolToObject(mode, "selected", modes[i].selected);
        cJSON_AddItemToArray(modes_json, mode);
    }
}

esp_err_t video_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_video_observation_t video;
    si_device_video_control_observation_t control;
    si_device_video_mode_observation_t *modes =
        device_http_calloc(SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES,
                           sizeof(*modes));
    size_t mode_count = 0;
    si_device_observation_get_video(&video, &control, modes,
                                    SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES,
                                    &mode_count);
    cJSON *root = cJSON_CreateObject();
    si_device_http_add_video_json(root, &video, &control, modes, mode_count);
    free(modes);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static const char *agent_tool_runtime_surface(const char *name)
{
    if (!name) {
        return "manual";
    }
    const char *const tool_calls[] = {
        "observe_status", "observe_video_status", "observe_hid_status",
        "memory_search", "history_search", "memory_write", "ask_user",
        "ssh_exec", "console_login", "host_display", "boot_key_sequence",
        "web_search",
        "uart_status", "uart_read", "uart_write", "uart_auth", "uart_baud",
        "power_action", "ensure_package", "ensure_user",
        "ensure_directory", "download_file", "write_file",
        "ensure_systemd_service", "check_service", "verify_port", "wait",
    };
    for (size_t i = 0;
         i < sizeof(tool_calls) / sizeof(tool_calls[0]); i++) {
        if (strcmp(name, tool_calls[i]) == 0) {
            return "tool_call";
        }
    }
    if (strcmp(name, "hid_actions") == 0) {
        return "action";
    }
    if (strcmp(name, "observe_screenshot") == 0) {
        return "context_attachment";
    }
    if (strcmp(name, "video_lease") == 0 ||
        strcmp(name, "control_lease") == 0) {
        return "runtime_manager";
    }
    return "manual";
}

static bool agent_tool_agent_callable(const char *name,
                                      const char *runtime_surface)
{
    return (runtime_surface &&
            (strcmp(runtime_surface, "tool_call") == 0 ||
             strcmp(runtime_surface, "action") == 0)) ||
           (name && strcmp(name, "observe_screenshot") == 0);
}

static bool agent_tool_mcp_callable(const char *name)
{
    if (!name) {
        return false;
    }
    static const char *const mapped[] = {
        "observe_status", "observe_screenshot", "video_lease",
        "control_lease", "hid_actions", "console_login", "power_action", "ssh_exec",
        "uart_status", "uart_read", "uart_write", "uart_auth", "uart_baud",
    };
    for (size_t i = 0; i < sizeof(mapped) / sizeof(mapped[0]); i++) {
        if (strcmp(name, mapped[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool agent_tool_policy_flag(const cJSON *policy_root,
                                   const char *tool_name,
                                   const char *field,
                                   const char *legacy_field)
{
    const cJSON *tools = cJSON_IsObject(policy_root) ?
                         cJSON_GetObjectItemCaseSensitive(
                             policy_root, "tools") : NULL;
    const cJSON *tool = cJSON_IsObject(tools) ?
                        cJSON_GetObjectItemCaseSensitive(
                            tools, tool_name) : NULL;
    const cJSON *enabled = cJSON_IsObject(tool) ?
                           cJSON_GetObjectItemCaseSensitive(
                               tool, field) : NULL;
    if (!cJSON_IsBool(enabled) && legacy_field && cJSON_IsObject(tool)) {
        enabled = cJSON_GetObjectItemCaseSensitive(tool, legacy_field);
    }
    return !cJSON_IsFalse(enabled);
}

static void add_agent_tool_json(cJSON *tools, const cJSON *policy_root,
                                const char *name, const char *category,
                                const char *method, const char *endpoint,
                                const char *permission, const char *risk,
                                bool available, bool implemented,
                                const char *description)
{
    cJSON *tool = cJSON_CreateObject();
    if (!tool) {
        return;
    }
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "category", category);
    cJSON_AddStringToObject(tool, "method", method);
    cJSON_AddStringToObject(tool, "endpoint", endpoint);
    cJSON_AddStringToObject(tool, "permission", permission);
    cJSON_AddStringToObject(tool, "risk", risk);
    cJSON_AddBoolToObject(tool, "available", available);
    cJSON_AddBoolToObject(tool, "implemented", implemented);
    const char *runtime_surface = agent_tool_runtime_surface(name);
    bool runtime_callable =
        SI_CFG_EMBEDDED_AGENT_ENABLED && implemented && runtime_surface &&
        (strcmp(runtime_surface, "tool_call") == 0 ||
         strcmp(runtime_surface, "action") == 0);
    bool agent_callable =
        SI_CFG_EMBEDDED_AGENT_ENABLED && implemented &&
        agent_tool_agent_callable(name, runtime_surface);
    bool mcp_callable =
        implemented && agent_tool_mcp_callable(name);
    bool agent_enabled =
        agent_callable &&
        agent_tool_policy_flag(policy_root, name, "agent_enabled",
                               "enabled");
    bool mcp_enabled =
        mcp_callable &&
        agent_tool_policy_flag(policy_root, name, "mcp_enabled", NULL);
    cJSON_AddStringToObject(tool, "runtime_surface", runtime_surface);
    cJSON_AddBoolToObject(tool, "runtime_callable", runtime_callable);
    cJSON_AddBoolToObject(tool, "agent_callable", agent_callable);
    cJSON_AddBoolToObject(tool, "mcp_callable", mcp_callable);
    cJSON_AddBoolToObject(tool, "agent_enabled", agent_enabled);
    cJSON_AddBoolToObject(tool, "mcp_enabled", mcp_enabled);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON_AddItemToArray(tools, tool);
}

static void add_agent_tool_contract_json(
    cJSON *root, const si_device_video_observation_t *video,
    const si_device_hid_observation_t *hid,
    const si_device_power_observation_t *power)
{
    cJSON *contract = cJSON_AddObjectToObject(root, "agent_tools");
    cJSON_AddStringToObject(contract, "schema_version", "exoanchor.agent_tools.v2");
    cJSON_AddNumberToObject(contract, "policy_schema_version", 2);
    cJSON_AddStringToObject(contract, "default_mode", "observe");
    cJSON_AddStringToObject(contract, "control_model", "observe | supervised | autonomous");

    char *policy_text =
        device_http_calloc(1, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
    cJSON *policy_root = NULL;
    if (policy_text) {
        (void)si_agent_tools_load_string(
            SI_AGENT_TOOLS_POLICY_KEY, "{\"version\":2,\"tools\":{}}",
            policy_text, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
        policy_root = cJSON_Parse(policy_text);
    }

    cJSON *tools = cJSON_AddArrayToObject(contract, "tools");
#define ADD_TOOL(...) add_agent_tool_json(tools, policy_root, __VA_ARGS__)
    ADD_TOOL("observe_status", "observe", "local", "runtime.observe_status",
                        "observe", "low", true, true,
                        "Read device, video, HID, power, lease, and performance state.");
    ADD_TOOL("observe_video_status", "observe", "local", "runtime.observe_video_status",
                        "observe", "low", true, true,
                        "Read video capture state, modes, owner, and errors.");
    ADD_TOOL("observe_hid_status", "observe", "local", "runtime.observe_hid_status",
                        "observe", "low", true, true,
                        "Read USB HID readiness and transport status.");
    ADD_TOOL("observe_screenshot", "observe", "GET", "/api/snapshot",
                        "observe", "low",
                        video ? video->enabled && video->initialized : false,
                        true,
                        "Capture the current HDMI frame as JPEG. The Agent runtime acquires the video lease and waits for the first frame when capture is idle.");
    ADD_TOOL("video_lease", "lease", "POST", "/api/video/lease",
                        "observe", "low", true, true,
                        "Acquire, renew, release, or take over video ownership for Agent preview and screenshots.");
    ADD_TOOL("control_lease", "lease", "GET/POST", "/api/control/lease",
                        "supervised", "medium", true, true,
                        "Acquire, renew, release, or inspect Agent HID control ownership.");
    ADD_TOOL("hid_actions", "act", "local", "runtime.hid_actions",
                        "supervised", "high", hid ? hid->ready : false, true,
                        "Execute ordered USB HID keyboard and mouse actions after Agent control lease.");
    si_console_credentials_status_t console_credentials = {0};
    bool console_login_available =
        si_console_credentials_get_status(&console_credentials) == ESP_OK &&
        console_credentials.configured &&
        console_credentials.password_configured &&
        hid && hid->ready;
    ADD_TOOL("console_login", "act", "local", "runtime.console_login",
                        "supervised", "high", console_login_available, true,
                        "Use console://default to inject a stored target-host username/password locally through USB HID without returning the secret.");
    ADD_TOOL("conversation_history", "memory", "GET/POST", "/api/agent/history",
                        "observe", "low", embedded_agent_storage_ready(), true,
                        "Read, append, or clear persistent Agent conversation records on TF card.");
    ADD_TOOL("working_memory", "memory", "GET/POST", "/api/agent/memory",
                        "observe", "low", embedded_agent_storage_ready(), true,
                        "Read, append, or clear persistent Agent task summaries and working notes on TF card.");
    ADD_TOOL("memory_search", "memory", "local", "runtime.memory_search",
                        "observe", "low", embedded_agent_storage_ready(), true,
                        "Search typed global working memory on TF card by query, type, session_id, and limit.");
    ADD_TOOL("history_search", "memory", "local", "runtime.history_search",
                        "observe", "low", embedded_agent_storage_ready(), true,
                        "Search persistent conversation history across all sessions or a selected session.");
    ADD_TOOL("memory_write", "memory", "local", "runtime.memory_write",
                        "supervised", "medium", embedded_agent_storage_ready(), true,
                        "Write durable typed memory records: fact, host_state, service_deployment, user_decision, todo, task_summary, or manual_note.");
    ADD_TOOL("power_action", "act", "local", "runtime.power_action",
                        "supervised", "high", power ? power->enabled : false, true,
                        "Press power, reset, or force-off GPIO controls when supported.");
    ADD_TOOL("wait", "runtime", "local", "runtime.wait",
                        "observe", "low", true, true,
                        "Pause 1..30000 ms between observations or action batches; callable as tool_calls wait with args.ms.");
    ADD_TOOL("ask_user", "runtime", "local", "runtime.ask_user",
                        "supervised", "medium", true, true,
                        "Ask the human for confirmation or missing information in the ESP32-P4 Agent.");
    bool ssh_available = si_web_ssh_target_available();
    ADD_TOOL("ssh_exec", "act", "local", "runtime.ssh_exec",
                        "supervised", "high", ssh_available, true,
                        "Execute a bounded SSH command on the Settings SSH target by default and return output, exit status, and timing.");
    ADD_TOOL("ensure_package", "act", "local", "runtime.ensure_package",
                        "supervised", "high", ssh_available, true,
                        "Install an exact bounded package set through the configured SSH target.");
    ADD_TOOL("ensure_user", "act", "local", "runtime.ensure_user",
                        "supervised", "high", ssh_available, true,
                        "Create or reconcile one target-host user through SSH.");
    ADD_TOOL("ensure_directory", "act", "local", "runtime.ensure_directory",
                        "supervised", "high", ssh_available, true,
                        "Create or reconcile one target-host directory through SSH.");
    ADD_TOOL("download_file", "act", "local", "runtime.download_file",
                        "supervised", "high", ssh_available, true,
                        "Download one file to the target host with optional SHA-256 verification.");
    ADD_TOOL("write_file", "act", "local", "runtime.write_file",
                        "supervised", "high", ssh_available, true,
                        "Write one bounded target-host file through SSH.");
    ADD_TOOL("ensure_systemd_service", "act", "local", "runtime.ensure_systemd_service",
                        "supervised", "high", ssh_available, true,
                        "Create or reconcile one systemd service through SSH.");
    ADD_TOOL("check_service", "observe", "local", "runtime.check_service",
                        "observe", "low", ssh_available, true,
                        "Read bounded target-host systemd service state through SSH.");
    ADD_TOOL("verify_port", "observe", "local", "runtime.verify_port",
                        "observe", "low", ssh_available, true,
                        "Verify one bounded target-host listening port through SSH.");
    ADD_TOOL("host_display", "act", "local", "runtime.host_display",
                        "supervised", "high", ssh_available, true,
                        "Observe Ubuntu DRM, create an exact display plan, require browser-session approval, persist a dedicated GRUB drop-in, verify read-back, or roll back.");
    ADD_TOOL("boot_key_sequence", "act", "local", "runtime.boot_key_sequence",
                        "supervised", "high",
                        hid ? hid->ready : false, true,
                        "Inspect or propose a bounded BIOS/Boot Menu key plan; browser approval, start, cancellation, and human KVM confirmation remain separate.");
    si_agent_web_search_settings_t web_search;
    si_agent_web_search_defaults(&web_search);
    (void)si_agent_web_search_load(&web_search);
    ADD_TOOL("web_search", "observe", "local", "runtime.web_search",
                        "observe", "low", si_agent_web_search_available(&web_search), true,
                        "Search the web through the configured Qwen Chat Completions provider with enable_search=true.");
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ADD_TOOL("ssh_terminal", "act", "WS", "/api/ws/ssh",
                        "supervised", "medium", ssh_available, true,
                        "Open the manual interactive SSH PTY; it is not a model tool call.");
#if SI_CFG_UART_TERMINAL_ENABLED
    ADD_TOOL("uart_terminal", "act", "WS", "/api/ws/uart",
                        "supervised", "medium", SI_CFG_TARGET_UART_ENABLED, true,
                        "Open the manual interactive target-host serial console; it is not a model tool call.");
#endif
#endif
    ADD_TOOL("uart_status", "observe", "local", "runtime.uart_status",
                        "observe", "low", SI_CFG_TARGET_UART_ENABLED, true,
                        "Read target UART initialization, baud, counters, journal cursor, and manual-terminal ownership.");
    ADD_TOOL("uart_write", "act", "local", "runtime.uart_write",
                        "supervised", "high", SI_CFG_TARGET_UART_ENABLED, true,
                        "Write exact bounded bytes to the target-host UART after Request Broker approval; manual Terminal has priority.");
    ADD_TOOL("uart_auth", "act", "local", "runtime.uart_auth",
                        "supervised", "high", SI_CFG_TARGET_UART_ENABLED, true,
                        "Submit a locally stored credential only after the retained UART tail matches an exact password prompt; secret material is never returned.");
    ADD_TOOL("uart_baud", "act", "local", "runtime.uart_baud",
                        "supervised", "high", SI_CFG_TARGET_UART_ENABLED, true,
                        "Switch target UART between primary and fallback baud modes after Request Broker approval.");
    ADD_TOOL("uart_read", "observe", "local", "runtime.uart_read",
                        "observe", "low", SI_CFG_TARGET_UART_ENABLED, true,
                        "Read the non-destructive target-host UART journal by cursor as text and base64.");

    cJSON *hid_schema = cJSON_AddObjectToObject(contract, "hid_action_schema");
    cJSON_AddStringToObject(hid_schema, "batch_endpoint", "/api/hid/actions");
    cJSON_AddStringToObject(hid_schema, "owner", "session-derived");
    cJSON_AddStringToObject(hid_schema, "browser_owner", "agent");
    cJSON_AddStringToObject(hid_schema, "mcp_owner", "mcp");
    cJSON_AddStringToObject(hid_schema, "embedded_agent_path",
                            "request_broker_execution_gateway");
    cJSON_AddNumberToObject(hid_schema, "max_items", HID_ACTIONS_MAX_ITEMS);
    cJSON *types = cJSON_AddArrayToObject(hid_schema, "allowed_types");
    const char *allowed[] = {
        "wait", "keydown", "keyup", "combo", "mousemove", "absmove",
        "absclick", "click", "wheel", "releaseall",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        cJSON_AddItemToArray(types, cJSON_CreateString(allowed[i]));
    }

    si_web_add_agent_tools_settings_json(contract);
#undef ADD_TOOL
    cJSON_Delete(policy_root);
    if (policy_text) {
        memset(policy_text, 0, SI_AGENT_TOOLS_POLICY_MAX_LEN + 1U);
        free(policy_text);
    }
}

void si_device_http_add_hid_json(cJSON *root,
                                 const si_device_hid_observation_t *hid)
{
    cJSON_AddBoolToObject(root, "enabled", hid->enabled);
    cJSON_AddBoolToObject(root, "initialized", hid->initialized);
    cJSON_AddBoolToObject(root, "connected", hid->mounted);
    cJSON_AddBoolToObject(root, "mounted", hid->mounted);
    cJSON_AddBoolToObject(root, "ready", hid->ready);
    cJSON_AddBoolToObject(root, "hid_ready", hid->ready);
    cJSON_AddStringToObject(root, "mode", hid->mode);
    cJSON_AddStringToObject(root, "port", hid->port);
    cJSON_AddStringToObject(root, "transport", hid->transport);

    cJSON *pins = cJSON_AddObjectToObject(root, "pins");
    cJSON_AddNumberToObject(pins, "dm_gpio", hid->dm_gpio);
    cJSON_AddNumberToObject(pins, "dp_gpio", hid->dp_gpio);

    cJSON *keyboard = cJSON_AddObjectToObject(root, "keyboard");
    cJSON_AddBoolToObject(keyboard, "available", hid->mounted);
    cJSON_AddBoolToObject(keyboard, "writable", hid->keyboard_writable);
    cJSON *mouse = cJSON_AddObjectToObject(root, "mouse");
    cJSON_AddBoolToObject(mouse, "available", hid->mounted);
    cJSON_AddBoolToObject(mouse, "writable", hid->mouse_writable);
    cJSON *absolute_pointer =
        cJSON_AddObjectToObject(root, "absolute_pointer");
    cJSON_AddBoolToObject(absolute_pointer, "available", hid->mounted);
    cJSON_AddBoolToObject(absolute_pointer, "writable",
                          hid->absolute_pointer_writable);

    cJSON *stats = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(stats, "tx_messages", hid->tx_messages);
    cJSON_AddNumberToObject(stats, "failed_messages", hid->failed_messages);
    cJSON_AddStringToObject(root, "last_error", hid->last_error);
}

esp_err_t hid_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_hid_observation_t hid;
    si_device_observation_get_hid(&hid);
    cJSON *root = cJSON_CreateObject();
    si_device_http_add_hid_json(root, &hid);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

void si_device_http_add_power_json(cJSON *root,
                                   const si_device_power_observation_t *power_status)
{
    cJSON_AddBoolToObject(root, "enabled", power_status->enabled);
    cJSON_AddBoolToObject(root, "initialized", power_status->initialized);
    cJSON_AddBoolToObject(root, "available",
                          power_status->enabled && power_status->initialized);
    cJSON_AddBoolToObject(root, "busy", power_status->busy);
    cJSON_AddBoolToObject(root, "active_high", power_status->active_high);
    cJSON_AddStringToObject(root, "active_level",
                            power_status->active_high ? "high" : "low");
    cJSON_AddNumberToObject(root, "default_press_ms", power_status->default_press_ms);
    cJSON_AddNumberToObject(root, "force_off_ms", power_status->force_off_ms);
    cJSON_AddNumberToObject(root, "action_count", power_status->action_count);
    cJSON_AddStringToObject(root, "last_action", power_status->last_action);
    cJSON_AddStringToObject(root, "last_error", power_status->last_error);

    cJSON *buttons = cJSON_AddObjectToObject(root, "buttons");
    cJSON *power = cJSON_AddObjectToObject(buttons, "power");
    cJSON_AddNumberToObject(power, "gpio", power_status->power_button_gpio);
    cJSON_AddStringToObject(power, "label", "Power button");
    cJSON_AddBoolToObject(power, "available",
                          power_status->enabled && power_status->initialized &&
                          power_status->power_button_gpio >= 0);
    cJSON_AddNumberToObject(power, "press_ms", power_status->default_press_ms);

    cJSON *reset = cJSON_AddObjectToObject(buttons, "reset");
    cJSON_AddNumberToObject(reset, "gpio", power_status->reset_button_gpio);
    cJSON_AddStringToObject(reset, "label", "Reset button");
    cJSON_AddBoolToObject(reset, "available",
                          power_status->enabled && power_status->initialized &&
                          power_status->reset_button_gpio >= 0);
    cJSON_AddNumberToObject(reset, "press_ms", power_status->default_press_ms);

    cJSON *detect = cJSON_AddObjectToObject(root, "detect");
    cJSON *power_detect = cJSON_AddObjectToObject(detect, "power");
    cJSON_AddNumberToObject(power_detect, "gpio", power_status->power_detect_gpio);
    cJSON_AddBoolToObject(power_detect, "supported", power_status->power_detect_supported);
    cJSON_AddBoolToObject(power_detect, "active", power_status->power_on);
    cJSON_AddBoolToObject(power_detect, "active_high",
                          power_status->power_detect_active_high);
    cJSON_AddStringToObject(power_detect, "active_level",
                            power_status->power_detect_active_high ? "high" : "low");
    cJSON_AddStringToObject(power_detect, "state",
                            power_status->power_detect_supported ?
                            (power_status->power_on ? "on" : "off") : "not-wired");
    cJSON *standby = cJSON_AddObjectToObject(detect, "standby");
    cJSON_AddNumberToObject(standby, "gpio", power_status->standby_detect_gpio);
    cJSON_AddBoolToObject(standby, "supported", power_status->standby_detect_supported);
    cJSON_AddBoolToObject(standby, "active", power_status->standby_on);
    cJSON_AddBoolToObject(standby, "active_high",
                          power_status->standby_detect_active_high);
    cJSON_AddStringToObject(standby, "active_level",
                            power_status->standby_detect_active_high ? "high" : "low");
    cJSON_AddStringToObject(standby, "state",
                            power_status->standby_detect_supported ?
                            (power_status->standby_on ? "on" : "off") : "not-wired");

    cJSON *locator = cJSON_AddObjectToObject(root, "locator");
    cJSON_AddNumberToObject(locator, "gpio", power_status->locator_gpio);
    cJSON_AddNumberToObject(locator, "return_gpio", power_status->locator_return_gpio);
    cJSON_AddBoolToObject(locator, "supported", power_status->locator_supported);
    cJSON_AddBoolToObject(locator, "active", power_status->locator_on);
    cJSON_AddBoolToObject(locator, "bidirectional", power_status->locator_bidirectional);
    cJSON_AddBoolToObject(locator, "reversed", power_status->locator_reversed);
    cJSON_AddBoolToObject(locator, "active_high", power_status->locator_active_high);
    cJSON_AddStringToObject(locator, "state",
                            power_status->locator_supported ?
                            (power_status->locator_on ? "on" : "off") : "not-wired");

    cJSON *map = cJSON_AddArrayToObject(root, "gpio_map");
    for (size_t i = 0; i < power_status->gpio_map_count &&
                       i < SI_DEVICE_OBSERVATION_MAX_POWER_GPIO; i++) {
        const si_device_power_gpio_observation_t *item = &power_status->gpio_map[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", item->role);
        cJSON_AddStringToObject(entry, "label", item->label);
        cJSON_AddStringToObject(entry, "direction", item->direction);
        cJSON_AddNumberToObject(entry, "gpio", item->gpio);
        cJSON_AddBoolToObject(entry, "enabled", item->enabled);
        cJSON_AddBoolToObject(entry, "implemented", item->implemented);
        cJSON_AddBoolToObject(entry, "active_high", item->active_high);
        cJSON_AddStringToObject(entry, "active_level", item->active_high ? "high" : "low");
        cJSON_AddBoolToObject(entry, "active", item->active);
        cJSON_AddBoolToObject(entry, "configurable", item->configurable);
        cJSON_AddBoolToObject(entry, "required", item->required);
        cJSON_AddStringToObject(entry, "state", item->state);
        cJSON_AddItemToArray(map, entry);
    }
}

void si_device_http_add_ms2109_power_json(
    cJSON *root, const si_device_ms2109_power_observation_t *power_status)
{
    cJSON_AddBoolToObject(root, "supported", power_status->supported);
    cJSON_AddBoolToObject(root, "initialized", power_status->initialized);
    cJSON_AddBoolToObject(root, "power_on", power_status->power_on);
    cJSON_AddStringToObject(root, "state_kind", power_status->state_kind);
    cJSON_AddBoolToObject(root, "rail_feedback_available",
                          power_status->rail_feedback_available);
    cJSON_AddNumberToObject(root, "switch_3v3_gpio",
                            power_status->switch_gpio);
    cJSON_AddNumberToObject(root, "switch_3v3_output_level",
                            power_status->switch_output_level);
    cJSON_AddNumberToObject(root, "enable_1v2_gpio",
                            power_status->core_enable_gpio);
    cJSON_AddNumberToObject(root, "enable_1v2_output_level",
                            power_status->core_enable_output_level);
    cJSON_AddNumberToObject(root, "operation_count",
                            power_status->operation_count);
    cJSON_AddStringToObject(root, "last_result", power_status->last_result);
}

void si_device_http_add_network_json(cJSON *root,
                                     const si_device_network_observation_t *network)
{
    cJSON_AddBoolToObject(root, "configured", network->configured);
    cJSON_AddBoolToObject(root, "connected", network->connected);
    cJSON_AddBoolToObject(root, "link_up", network->link_up);
    cJSON_AddBoolToObject(root, "full_duplex", network->full_duplex);
    cJSON_AddBoolToObject(root, "pending_confirmation",
                          network->pending_confirmation);
    cJSON_AddBoolToObject(root, "recovery_active",
                          network->recovery_active);
    cJSON_AddNumberToObject(root, "speed_mbps", network->speed_mbps);
    cJSON_AddNumberToObject(root, "config_generation",
                            network->config_generation);
    cJSON_AddNumberToObject(root, "confirm_remaining_seconds",
                            network->confirm_remaining_seconds);
    cJSON_AddStringToObject(root, "interface", network->interface);
    cJSON_AddStringToObject(root, "driver", network->driver);
    cJSON_AddStringToObject(root, "device_id", network->device_id);
    cJSON_AddStringToObject(root, "hostname", network->hostname);
    cJSON_AddStringToObject(root, "mode", network->mode);
    cJSON_AddStringToObject(root, "address_source",
                            network->address_source);
    cJSON_AddStringToObject(root, "config_state",
                            network->config_state);
    cJSON_AddStringToObject(root, "ip", network->ip);
    cJSON_AddStringToObject(root, "netmask", network->netmask);
    cJSON_AddStringToObject(root, "gateway", network->gateway);
    cJSON_AddStringToObject(root, "mac", network->mac);
}

static void add_heap_json(cJSON *root, const char *name,
                          const si_device_heap_observation_t *heap_status)
{
    cJSON *heap = cJSON_AddObjectToObject(root, name);
    cJSON_AddNumberToObject(heap, "total_bytes", heap_status->total_bytes);
    cJSON_AddNumberToObject(heap, "used_bytes", heap_status->used_bytes);
    cJSON_AddNumberToObject(heap, "free_bytes", heap_status->free_bytes);
    cJSON_AddNumberToObject(heap, "minimum_free_bytes", heap_status->minimum_free_bytes);
    cJSON_AddNumberToObject(heap, "largest_free_block", heap_status->largest_free_block);
    cJSON_AddNumberToObject(heap, "usage_percent",
                            si_observation_used_percent(heap_status->total_bytes,
                                                        heap_status->free_bytes));
}

void si_device_http_add_performance_json(
    cJSON *root, const si_device_performance_observation_t *performance,
    int active_connections)
{
    const si_device_video_observation_t *video = &performance->video;
    double fps = video->fps_x100 / 100.0;
    double target_fps = video->target_fps_x100 / 100.0;
    double estimated_mbps = fps * (double)video->last_jpeg_size * 8.0 / 1000000.0;
    uint32_t frame_total = video->frames_captured;
    if (frame_total == 0) {
        frame_total = video->frames_encoded + video->frames_dropped;
    }
    double drop_percent = frame_total == 0 ? 0.0 :
                          ((double)video->frames_dropped * 100.0) / (double)frame_total;

    cJSON_AddNumberToObject(root, "uptime_seconds", performance->uptime_seconds);
    cJSON_AddNumberToObject(root, "active_connections", active_connections);
    cJSON_AddNumberToObject(root, "task_count", performance->task_count);

    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddBoolToObject(cpu, "valid", performance->cpu.valid);
    cJSON_AddNumberToObject(cpu, "cores", performance->cpu.core_count);
    cJSON_AddNumberToObject(cpu, "usage_percent", performance->cpu.overall_percent);
    cJSON_AddNumberToObject(cpu, "sample_window_ms", performance->cpu.sample_window_ms);
    (void)cJSON_AddArrayToObject(cpu, "core_usage_percent");

    cJSON *memory = cJSON_AddObjectToObject(root, "memory");
    add_heap_json(memory, "heap", &performance->heap);
    add_heap_json(memory, "internal", &performance->internal_heap);
    add_heap_json(memory, "psram", &performance->psram);

    cJSON *storage = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddStringToObject(storage, "schema_version", "exoanchor.storage.v1");
    add_flash_storage_json(storage, "flash", &performance->flash);
    add_tf_storage_json(storage, "tf_card", &performance->tf);
    cJSON *storage_devices = cJSON_AddArrayToObject(storage, "devices");
    add_storage_device_summary(storage_devices, "internal_flash", "Flash",
                               "nor_flash", "internal_spi", "firmware", true,
                               performance->flash.detected, false,
                               performance->flash.total_bytes,
                               performance->flash.partition_bytes,
                               performance->flash.free_bytes,
                               si_observation_used_percent(performance->flash.total_bytes,
                                                           performance->flash.free_bytes),
                               "");
    add_storage_device_summary(storage_devices, "tf_card", "TF Card",
                               "removable_card", "sdmmc", "data",
                               performance->tf.supported, performance->tf.mounted,
                               performance->tf.mounted, performance->tf.total_bytes,
                               performance->tf.used_bytes, performance->tf.free_bytes,
                               performance->tf.usage_percent,
                               performance->tf.last_error);
    cJSON *expansion_slots = cJSON_AddArrayToObject(storage, "expansion_slots");
    add_storage_expansion_slot(expansion_slots, "spi_data", "SPI Data",
                               "spi", "data");

    cJSON *stream = cJSON_AddObjectToObject(root, "video");
    cJSON_AddNumberToObject(stream, "fps", fps);
    cJSON_AddNumberToObject(stream, "target_fps", target_fps);
    cJSON_AddNumberToObject(stream, "estimated_mbps", estimated_mbps);
    cJSON_AddNumberToObject(stream, "last_jpeg_size", video->last_jpeg_size);
    cJSON_AddNumberToObject(stream, "frame_interval_ms",
                            video->frame_interval_ms);
    cJSON_AddNumberToObject(stream, "expected_frame_interval_ms",
                            target_fps > 0.0 ? 1000.0 / target_fps : 0.0);
    cJSON_AddNumberToObject(stream, "frames_captured", video->frames_captured);
    cJSON_AddNumberToObject(stream, "frames_encoded", video->frames_encoded);
    cJSON_AddNumberToObject(stream, "frames_dropped", video->frames_dropped);
    cJSON_AddNumberToObject(stream, "drop_percent", drop_percent);
}

static void add_session_capabilities_json(
    cJSON *root, const si_auth_session_context_t *session)
{
    cJSON *identity = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(identity, "id", session->session_id);
    cJSON_AddStringToObject(identity, "principal",
                            si_principal_kind_name(session->principal));
    cJSON_AddBoolToObject(identity, "authenticated", session->authenticated);
    cJSON *capabilities =
        cJSON_AddArrayToObject(identity, "capabilities");
    static const si_capability_t known[] = {
        SI_CAPABILITY_OBSERVE,
        SI_CAPABILITY_SSH,
        SI_CAPABILITY_UART,
        SI_CAPABILITY_HID,
        SI_CAPABILITY_POWER,
        SI_CAPABILITY_SETTINGS,
        SI_CAPABILITY_OTA,
        SI_CAPABILITY_AGENT_RUN,
        SI_CAPABILITY_CONTROL_LEASE,
        SI_CAPABILITY_STORAGE,
        SI_CAPABILITY_VIDEO_CONTROL,
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (si_capability_set_has(session->capabilities, known[i])) {
            cJSON_AddItemToArray(
                capabilities,
                cJSON_CreateString(si_capability_name(known[i])));
        }
    }
}

static void add_gpio_matrix_assignment(cJSON *assignments, int gpio,
                                       const char *signal,
                                       const char *label,
                                       const char *subsystem,
                                       const char *direction)
{
    if (!assignments || gpio < 0 || gpio > 54) {
        return;
    }
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "gpio", gpio);
    cJSON_AddStringToObject(entry, "signal", signal);
    cJSON_AddStringToObject(entry, "label", label);
    cJSON_AddStringToObject(entry, "subsystem", subsystem);
    cJSON_AddStringToObject(entry, "direction", direction);
    cJSON_AddBoolToObject(entry, "configurable", false);
    cJSON_AddStringToObject(entry, "source", "board_profile");
    cJSON_AddItemToArray(assignments, entry);
}

static void add_gpio_matrix_json(cJSON *root)
{
    cJSON *matrix = cJSON_AddObjectToObject(root, "gpio_matrix");
    cJSON_AddNumberToObject(matrix, "gpio_min", 0);
    cJSON_AddNumberToObject(matrix, "gpio_max", 54);
    cJSON_AddStringToObject(
        matrix, "scope",
        "Firmware-owned numbered GPIOs; dedicated USB, flash, and PSRAM pads are excluded.");
    cJSON_AddStringToObject(matrix, "runtime_overlay", "power.gpio_map");
    cJSON *assignments = cJSON_AddArrayToObject(matrix, "assignments");

    if (SI_CFG_ETH_ENABLED) {
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_MDC_GPIO,
                                   "eth_mdc", "RMII MDC", "ethernet", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_MDIO_GPIO,
                                   "eth_mdio", "RMII MDIO", "ethernet", "inout");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_PHY_RESET_GPIO,
                                   "eth_phy_reset", "PHY RESET", "ethernet", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_CLOCK_GPIO,
                                   "eth_rmii_clk", "RMII REF_CLK", "ethernet", "input");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_TX_ENABLE_GPIO,
                                   "eth_rmii_tx_en", "RMII TX_EN", "ethernet", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_TXD0_GPIO,
                                   "eth_rmii_txd0", "RMII TXD0", "ethernet", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_TXD1_GPIO,
                                   "eth_rmii_txd1", "RMII TXD1", "ethernet", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_CRS_DV_GPIO,
                                   "eth_rmii_crs_dv", "RMII CRS_DV", "ethernet", "input");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_RXD0_GPIO,
                                   "eth_rmii_rxd0", "RMII RXD0", "ethernet", "input");
        add_gpio_matrix_assignment(assignments, SI_CFG_ETH_RMII_RXD1_GPIO,
                                   "eth_rmii_rxd1", "RMII RXD1", "ethernet", "input");
    }
    if (SI_CFG_HID_ENABLED) {
        add_gpio_matrix_assignment(assignments, SI_CFG_HID_DM_GPIO,
                                   "hid_usb_dm", "USB HID D-", "usb_hid", "inout");
        add_gpio_matrix_assignment(assignments, SI_CFG_HID_DP_GPIO,
                                   "hid_usb_dp", "USB HID D+", "usb_hid", "inout");
    }
    add_gpio_matrix_assignment(assignments, USB_USJ_INT_PHY_DM_GPIO_NUM,
                               "usb_host_dm", "USB HOST D-", "usb_host", "inout");
    add_gpio_matrix_assignment(assignments, USB_USJ_INT_PHY_DP_GPIO_NUM,
                               "usb_host_dp", "USB HOST D+", "usb_host", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_CARD_DETECT_GPIO,
                               "tf_card_detect", "TF CARD DETECT", "storage", "input");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_CLK_GPIO,
                               "tf_sdmmc_clk", "TF SDMMC CLK", "storage", "output");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_CMD_GPIO,
                               "tf_sdmmc_cmd", "TF SDMMC CMD", "storage", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_D0_GPIO,
                               "tf_sdmmc_d0", "TF SDMMC D0", "storage", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_D1_GPIO,
                               "tf_sdmmc_d1", "TF SDMMC D1", "storage", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_D2_GPIO,
                               "tf_sdmmc_d2", "TF SDMMC D2", "storage", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_TF_SDMMC_D3_GPIO,
                               "tf_sdmmc_d3", "TF SDMMC D3", "storage", "inout");
    if (SI_CFG_TARGET_UART_ENABLED) {
        add_gpio_matrix_assignment(assignments, SI_CFG_TARGET_UART_RX_GPIO,
                                   "target_uart_rx", "UART1 RX", "target_uart", "input");
        add_gpio_matrix_assignment(assignments, SI_CFG_TARGET_UART_TX_GPIO,
                                   "target_uart_tx", "UART1 TX", "target_uart", "output");
    }
    add_gpio_matrix_assignment(assignments, SI_CFG_MS2109_EEPROM_WP_GPIO,
                               "ms2109_eeprom_wp", "MS2109 EEPROM WP", "ms2109", "output");
    if (SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED) {
        add_gpio_matrix_assignment(assignments, SI_CFG_MS2109_SWITCH_GPIO,
                                   "ms2109_power_switch", "MS2109 3V3 POWER", "ms2109", "output");
        add_gpio_matrix_assignment(assignments, SI_CFG_MS2109_CORE_ENABLE_GPIO,
                                   "ms2109_core_enable", "MS2109 1V2 ENABLE", "ms2109", "output");
    }
    add_gpio_matrix_assignment(assignments, SI_CFG_MS2109_EEPROM_SCL_GPIO,
                               "ms2109_eeprom_scl", "MS2109 EEPROM SCL", "ms2109", "inout");
    add_gpio_matrix_assignment(assignments, SI_CFG_MS2109_EEPROM_SDA_GPIO,
                               "ms2109_eeprom_sda", "MS2109 EEPROM SDA", "ms2109", "inout");
    add_gpio_matrix_assignment(assignments, U0RXD_GPIO_NUM,
                               "debug_uart_rx", "DEBUG UART RX", "debug", "input");
    add_gpio_matrix_assignment(assignments, U0TXD_GPIO_NUM,
                               "debug_uart_tx", "DEBUG UART TX", "debug", "output");
}

esp_err_t capabilities_handler(httpd_req_t *req)
{
    si_auth_session_context_t session;
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_device_video_observation_t video;
    si_device_video_control_observation_t video_control;
    si_device_hid_observation_t hid;
    si_device_power_observation_t power;
    si_device_system_observation_t system;
    si_device_observation_get_video(&video, &video_control, NULL, 0, NULL);
    si_device_observation_get_hid(&hid);
    si_device_observation_get_power(&power);
    si_device_observation_get_system(&system);
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 2);
    cJSON *build = cJSON_AddObjectToObject(root, "build");
    cJSON_AddStringToObject(build, "profile", SI_BUILD_PROFILE);
    cJSON_AddBoolToObject(build, "embedded_agent",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(build, "external_mcp", true);
    add_session_capabilities_json(root, &session);
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "family", "ESP32P4");
    cJSON_AddStringToObject(device, "board", SI_BOARD_ID);
    cJSON_AddStringToObject(device, "board_name", SI_BOARD_NAME);
    cJSON_AddStringToObject(device, "silicon_target", SI_SILICON_TARGET);
    cJSON_AddStringToObject(device, "label", system.device_label);
    cJSON_AddStringToObject(device, "hostname", system.hostname);
    cJSON_AddStringToObject(device, "firmware_version", app ? app->version : "");
    cJSON_AddStringToObject(device, "build_profile", SI_BUILD_PROFILE);
    add_gpio_matrix_json(root);

    cJSON *agent = cJSON_AddObjectToObject(root, "agent_api");
    cJSON_AddNumberToObject(agent, "version", 1);
    cJSON_AddStringToObject(
        agent, "runtime_owner",
        SI_CFG_EMBEDDED_AGENT_ENABLED ? "embedded_esp32p4" : "external_mcp");
    cJSON_AddStringToObject(
        agent, "embedded_runtime_lifecycle",
        SI_CFG_EMBEDDED_AGENT_ENABLED ? "active" : "excluded");
    cJSON_AddBoolToObject(agent, "embedded_runtime_new_features",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(agent, "embedded_runtime",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(agent, "capabilities", true);
    cJSON_AddBoolToObject(agent, "video_lease_owner_agent", true);
    cJSON_AddBoolToObject(agent, "control_lease", true);
    cJSON_AddNumberToObject(agent, "control_lease_ms", SI_CONTROL_LEASE_GRACE_MS);
    cJSON_AddBoolToObject(agent, "human_kvm_preempts_agent", true);
    cJSON_AddBoolToObject(agent,
                          "kvm_observation_shared_during_input_control", true);
    cJSON_AddBoolToObject(agent, "manual_stop_control", true);
    cJSON_AddBoolToObject(agent, "hid_actions", true);
    cJSON_AddBoolToObject(agent, "api_settings_nvs",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(agent, "system_prompt_nvs",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(agent, "tool_policy_nvs", true);
    cJSON_AddBoolToObject(agent, "device_cloud_run",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    cJSON_AddBoolToObject(agent, "screenshot_opt_in",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);
    bool agent_storage = embedded_agent_storage_ready();
    cJSON_AddBoolToObject(agent, "conversation_history", agent_storage);
    cJSON_AddBoolToObject(agent, "conversation_sessions", agent_storage);
    cJSON_AddBoolToObject(agent, "working_memory", agent_storage);
    cJSON_AddStringToObject(
        agent, "conversation_storage",
        SI_CFG_EMBEDDED_AGENT_ENABLED ? "tf_card" : "none");
    cJSON *modes = cJSON_AddArrayToObject(agent, "control_modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("observe"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("supervised"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("autonomous"));

    cJSON *settings_caps = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddBoolToObject(settings_caps, "nvs", true);
    cJSON_AddBoolToObject(settings_caps, "session", true);
    cJSON_AddBoolToObject(settings_caps, "video", true);
    cJSON_AddBoolToObject(settings_caps, "console_credentials", true);
    cJSON_AddBoolToObject(settings_caps, "agent_tools", true);
    cJSON_AddBoolToObject(settings_caps, "target_profile", true);
    cJSON_AddBoolToObject(settings_caps, "agent_display_name",
                          SI_CFG_EMBEDDED_AGENT_ENABLED);

    cJSON *video_caps = cJSON_AddObjectToObject(root, "video");
    cJSON_AddBoolToObject(video_caps, "enabled", video.enabled);
    cJSON_AddBoolToObject(video_caps, "initialized", video.initialized);
    bool video_device_connected = video.modes_count > 0;
    cJSON_AddBoolToObject(video_caps, "connected",
                          video_device_connected);
    cJSON_AddBoolToObject(video_caps, "device_connected",
                          video_device_connected);
    cJSON_AddBoolToObject(video_caps, "frame_ready", video.frame_ready);
    cJSON_AddStringToObject(
        video_caps, "state",
        !video.enabled ? "disabled" :
        video.frame_ready ? "active" :
        video.modes_count > 0 ? "standby" :
        video.initialized ? "waiting" : "offline");
    cJSON_AddBoolToObject(video_caps, "snapshot", true);
    cJSON_AddBoolToObject(video_caps, "mjpeg_stream", true);
#if SI_CFG_VIDEO_H264_ENABLED
    si_h264_stream_status_t h264 = {0};
    si_h264_stream_get_status(&h264);
    cJSON_AddBoolToObject(video_caps, "h264_compiled", true);
    cJSON_AddBoolToObject(video_caps, "h264_resource_probe_attempted",
                          h264.resource_probe_attempted);
    cJSON_AddBoolToObject(video_caps, "h264_runtime_available",
                          h264.available);
    cJSON_AddBoolToObject(video_caps,
                          "h264_reference_workspace_reserved",
                          h264.reference_workspace_reserved);
    cJSON_AddStringToObject(
        video_caps, "h264_reference_workspace_residency",
        h264.reference_workspace_reserved
            ? (h264.reference_workspace_internal ? "internal" : "psram")
            : "none");
    cJSON_AddBoolToObject(video_caps, "h264_restore_pending",
                          h264.restore_pending);
    cJSON_AddStringToObject(video_caps, "h264_resource_error",
                            esp_err_to_name(h264.resource_error));
#else
    cJSON_AddBoolToObject(video_caps, "h264_compiled", false);
    cJSON_AddBoolToObject(video_caps, "h264_resource_probe_attempted", false);
    cJSON_AddBoolToObject(video_caps, "h264_runtime_available", false);
    cJSON_AddBoolToObject(video_caps,
                          "h264_reference_workspace_reserved", false);
    cJSON_AddStringToObject(video_caps,
                            "h264_reference_workspace_residency", "none");
    cJSON_AddBoolToObject(video_caps, "h264_restore_pending", false);
    cJSON_AddStringToObject(video_caps, "h264_resource_error",
                            "ESP_ERR_NOT_SUPPORTED");
#endif
    cJSON_AddBoolToObject(video_caps, "lease", true);
    cJSON_AddBoolToObject(video_caps, "agent_low_fps_lease", true);
    cJSON_AddBoolToObject(video_caps, "settings_nvs", true);
    cJSON_AddNumberToObject(video_caps, "preview_width", video_control.preview_width);
    cJSON_AddNumberToObject(video_caps, "preview_height", video_control.preview_height);
    cJSON_AddNumberToObject(video_caps, "preview_fps", video_control.preview_fps_x100 / 100.0);

    cJSON *hid_caps = cJSON_AddObjectToObject(root, "hid");
    cJSON_AddBoolToObject(hid_caps, "enabled", hid.enabled);
    cJSON_AddBoolToObject(hid_caps, "initialized", hid.initialized);
    cJSON_AddBoolToObject(hid_caps, "ready", hid.ready);
    cJSON_AddBoolToObject(hid_caps, "keyboard", true);
    cJSON_AddBoolToObject(hid_caps, "relative_mouse", true);
    cJSON_AddBoolToObject(hid_caps, "absolute_mouse", true);
    cJSON_AddBoolToObject(hid_caps, "wheel", true);
    cJSON_AddBoolToObject(hid_caps, "websocket", true);
    cJSON_AddBoolToObject(hid_caps, "batch_actions", true);

    cJSON *power_caps = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(power_caps, "enabled", power.enabled);
    cJSON_AddBoolToObject(power_caps, "initialized", power.initialized);
    cJSON_AddBoolToObject(power_caps, "power_button", power.power_button_gpio >= 0);
    cJSON_AddBoolToObject(power_caps, "reset_button", power.reset_button_gpio >= 0);
    cJSON_AddBoolToObject(power_caps, "power_detect", power.power_detect_supported);
    cJSON_AddBoolToObject(power_caps, "standby_detect", power.standby_detect_supported);
    cJSON_AddBoolToObject(power_caps, "locator", power.locator_supported);

#if SI_CFG_MS2109_TEST_ENABLED
    si_ms2109_test_status_t ms2109_power_status;
    si_ms2109_test_get_status(&ms2109_power_status);
#elif SI_CFG_MS2109_POWER_ENABLED
    si_ms2109_power_status_t ms2109_power_status;
    (void)si_ms2109_power_get_status(&ms2109_power_status);
#else
    si_ms2109_power_status_t ms2109_power_status = {
        .enabled = false,
        .switch_gpio = -1,
        .core_enable_gpio = -1,
        .switch_output_level = -1,
        .core_enable_output_level = -1,
        .last_result = ESP_ERR_NOT_SUPPORTED,
    };
#endif
    cJSON *ms2109_power_caps = cJSON_AddObjectToObject(root, "ms2109_power");
    cJSON_AddBoolToObject(ms2109_power_caps, "supported",
                          ms2109_power_status.enabled);
    cJSON_AddBoolToObject(ms2109_power_caps, "initialized",
                          ms2109_power_status.initialized);
    cJSON_AddBoolToObject(ms2109_power_caps, "power_control",
                          ms2109_power_status.enabled);
    cJSON_AddBoolToObject(ms2109_power_caps, "power_cycle",
                          ms2109_power_status.enabled);
    cJSON_AddStringToObject(ms2109_power_caps, "status_endpoint",
                            ms2109_power_status.enabled ?
                            "/api/ms2109/status" : "");

#if SI_CFG_MS2109_TEST_ENABLED
    si_ms2109_test_status_t ms2109_status = ms2109_power_status;
    cJSON *ms2109_caps = cJSON_AddObjectToObject(root, "ms2109_test");
    cJSON_AddBoolToObject(ms2109_caps, "supported",
                          ms2109_status.enabled);
    cJSON_AddBoolToObject(ms2109_caps, "initialized",
                          ms2109_status.initialized);
    cJSON_AddBoolToObject(ms2109_caps, "power_control",
                          ms2109_status.enabled);
    cJSON_AddBoolToObject(ms2109_caps, "physical_eeprom",
                          ms2109_status.enabled);
    cJSON_AddBoolToObject(ms2109_caps, "eeprom_read",
                          ms2109_status.enabled);
    cJSON_AddBoolToObject(ms2109_caps, "eeprom_program_and_verify",
                          ms2109_status.enabled);
    cJSON_AddStringToObject(ms2109_caps, "status_endpoint",
                            ms2109_status.enabled ?
                            "/api/ms2109/status" : "");
#endif

    cJSON *uart_caps = cJSON_AddObjectToObject(root, "uart");
    cJSON_AddBoolToObject(uart_caps, "supported", SI_CFG_TARGET_UART_ENABLED);
    cJSON_AddNumberToObject(uart_caps, "port", SI_CFG_TARGET_UART_PORT);
    cJSON_AddNumberToObject(uart_caps, "rx_gpio", SI_CFG_TARGET_UART_RX_GPIO);
    cJSON_AddNumberToObject(uart_caps, "tx_gpio", SI_CFG_TARGET_UART_TX_GPIO);
    cJSON_AddNumberToObject(uart_caps, "baud_rate",
                            SI_CFG_TARGET_UART_BAUD_RATE);
    cJSON_AddNumberToObject(uart_caps, "fallback_baud_rate",
                            SI_CFG_TARGET_UART_FALLBACK_BAUD_RATE);
    cJSON_AddStringToObject(uart_caps, "format", "8N1");
    cJSON_AddBoolToObject(uart_caps, "websocket",
                          SI_CFG_UART_TERMINAL_ENABLED);
    cJSON_AddBoolToObject(uart_caps, "manual_terminal",
                          SI_CFG_UART_TERMINAL_ENABLED);
    cJSON_AddStringToObject(uart_caps, "note",
                            !SI_CFG_TARGET_UART_ENABLED ?
                            "Target UART bridge is not enabled on this board profile." :
                            SI_CFG_UART_TERMINAL_ENABLED ?
                            "Target UART bridge and manual Web terminal are enabled." :
                            "Target UART is available to authenticated HTTP/MCP operations; the manual Web terminal is excluded from this build.");

    cJSON *observations = cJSON_AddObjectToObject(root, "observations");
    cJSON_AddStringToObject(observations, "device_status", "/api/status");
    cJSON_AddStringToObject(observations, "power_status", "/api/power/status");
    cJSON_AddStringToObject(observations, "hid_status", "/api/hid/status");
    cJSON_AddStringToObject(observations, "uart_status", "/api/uart/status");
    cJSON_AddStringToObject(observations, "snapshot", "/api/snapshot");
    cJSON_AddStringToObject(observations, "control_lease", "/api/control/lease");

    add_agent_tool_contract_json(root, &video, &hid, &power);

    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

void si_device_http_add_control_lease_json(
    cJSON *root, const si_device_control_lease_observation_t *lease)
{
    cJSON_AddStringToObject(root, "owner", lease->owner);
    cJSON_AddStringToObject(root, "mode", lease->mode);
    cJSON_AddBoolToObject(root, "active", lease->active);
    cJSON_AddBoolToObject(root, "kvm_view_active", lease->kvm_view_active);
    cJSON_AddBoolToObject(root, "kvm_active", lease->kvm_active);
    cJSON_AddNumberToObject(root, "kvm_expires_in_ms", lease->kvm_remaining_ms);
    cJSON_AddBoolToObject(root, "agent_active", lease->agent_active);
    cJSON_AddBoolToObject(root, "input_control_active",
                          lease->input_control_active);
    cJSON_AddStringToObject(root, "agent_owner", lease->agent_owner);
    cJSON_AddStringToObject(root, "session_id", lease->session_id);
    cJSON_AddBoolToObject(root, "agent_takeover", lease->agent_takeover);
    cJSON_AddNumberToObject(root, "agent_expires_in_ms", lease->agent_remaining_ms);
    cJSON_AddNumberToObject(root, "expires_in_ms", lease->expires_in_ms);
    cJSON_AddNumberToObject(root, "lease_ms", lease->lease_ms);
    cJSON_AddBoolToObject(root, "can_request", lease->can_request);
    cJSON_AddStringToObject(root, "reason", lease->reason);
}

static void add_current_control_lease_json(cJSON *root)
{
    si_device_control_lease_observation_t lease;
    si_device_observation_get_control_lease(&lease);
    si_device_http_add_control_lease_json(root, &lease);
}

static const char *control_owner_for_session(
    const si_auth_session_context_t *session)
{
    if (!session) {
        return NULL;
    }
    if (session->principal == SI_PRINCIPAL_MCP) {
        return "mcp";
    }
    if (session->principal == SI_PRINCIPAL_BROWSER) {
        return "agent";
    }
    return NULL;
}

static si_authorization_mode_t control_authorization_mode(const char *mode)
{
    if (mode && strcasecmp(mode, "autonomous") == 0) {
        return SI_AUTHZ_MODE_AUTONOMOUS;
    }
    if (mode && strcasecmp(mode, "supervised") == 0) {
        return SI_AUTHZ_MODE_SUPERVISED;
    }
    return SI_AUTHZ_MODE_OBSERVE;
}

static bool control_lease_auth_live(void *opaque)
{
    const si_auth_session_context_t *expected = opaque;
    si_auth_session_context_t live = {0};
    return expected && expected->session_id[0] &&
        expected->generation != 0U &&
        si_auth_session_get_live_context(expected->session_id,
                                         expected->generation, &live) &&
        live.authenticated && live.principal == expected->principal &&
        live.generation == expected->generation &&
        strcmp(live.session_id, expected->session_id) == 0 &&
        si_capability_set_has(live.capabilities,
                              SI_CAPABILITY_CONTROL_LEASE);
}

static bool direct_hid_principal_allowed(si_principal_kind_t principal)
{
    /* Browser-originated supervised batches and the separately authenticated
     * external MCP controller use this transport.  The embedded Agent remains
     * behind Request Broker + Execution Gateway and never receives an HTTP
     * auth session for this path. */
    return principal == SI_PRINCIPAL_BROWSER ||
           principal == SI_PRINCIPAL_MCP;
}

static bool hid_actions_control_checkpoint(
    const si_auth_session_context_t *session, const char *owner,
    uint32_t expected_lease_epoch, const char **denial_reason)
{
    const char *reason = NULL;
    if (!session || !direct_hid_principal_allowed(session->principal) ||
        !owner || !session->session_id[0] || session->generation == 0U ||
        expected_lease_epoch == 0U) {
        reason = "invalid direct HID control context";
    } else {
        si_auth_session_context_t live_session;
        if (!si_auth_session_get_live_context(
                session->session_id, session->generation, &live_session)) {
            reason = "HID session was revoked or expired";
        } else if (!live_session.authenticated ||
                   !direct_hid_principal_allowed(live_session.principal) ||
                   live_session.principal != session->principal ||
                   live_session.generation != session->generation ||
                   strcmp(live_session.session_id, session->session_id) != 0 ||
                   !si_capability_set_has(live_session.capabilities,
                                          SI_CAPABILITY_HID)) {
            reason = "session no longer has HID capability";
        }

        si_control_lease_status_t lease = {0};
        if (!reason) {
            si_control_lease_get_status(&lease);
        }
        if (!reason && lease.kvm_active) {
            reason = "manual KVM input preempted direct HID actions";
        } else if (!reason && lease.epoch != expected_lease_epoch) {
            reason = "control lease epoch changed during HID actions";
        } else if (!reason &&
                   (!lease.agent_active ||
                   strcasecmp(lease.agent_owner, owner) != 0 ||
                   strcmp(lease.session_id, session->session_id) != 0 ||
                   !si_control_lease_owner_session_matches(
                       owner, session->session_id))) {
            reason = "control lease owner or session changed";
        } else if (!reason && control_authorization_mode(lease.mode) ==
                   SI_AUTHZ_MODE_OBSERVE) {
            reason = "control lease became observe-only";
        }
    }
    if (denial_reason) {
        *denial_reason = reason;
    }
    return reason == NULL;
}

typedef struct {
    const si_auth_session_context_t *session;
    const char *owner;
    uint32_t expected_lease_epoch;
    const char *denial_reason;
} hid_actions_authority_guard_t;

static bool hid_actions_authority_guard(void *opaque)
{
    hid_actions_authority_guard_t *guard = opaque;
    if (!guard) {
        return false;
    }
    guard->denial_reason = NULL;
    return hid_actions_control_checkpoint(
        guard->session, guard->owner, guard->expected_lease_epoch,
        &guard->denial_reason);
}

static bool hid_actions_guarded_wait(
    const si_auth_session_context_t *session, const char *owner,
    uint32_t expected_lease_epoch, uint32_t wait_ms,
    const char **denial_reason)
{
    uint32_t remaining_ms = wait_ms;
    while (remaining_ms > 0U) {
        if (!hid_actions_control_checkpoint(
                session, owner, expected_lease_epoch, denial_reason)) {
            return false;
        }
        uint32_t slice_ms = remaining_ms > HID_ACTIONS_WAIT_SLICE_MS ?
                            HID_ACTIONS_WAIT_SLICE_MS : remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        remaining_ms -= slice_ms;
    }
    return true;
}

static bool hid_actions_release_all(
    const si_auth_session_context_t *session, const char *owner,
    uint32_t expected_lease_epoch)
{
    si_hid_owner_token_t hid_owner = {0};
    if (!si_control_lease_get_hid_owner(
            owner, session->session_id, session->generation,
            expected_lease_epoch, &hid_owner)) {
        return false;
    }
    hid_actions_authority_guard_t guard = {
        .session = session,
        .owner = owner,
        .expected_lease_epoch = expected_lease_epoch,
    };
    const si_hid_command_t release = {.type = SI_HID_COMMAND_RELEASE_ALL};
    return si_hid_execute_owned(&release, &hid_owner,
                                hid_actions_authority_guard, &guard) == ESP_OK;
}

static void hid_actions_record_control_abort(cJSON *response, cJSON *results,
                                             int index, const char *reason,
                                             const si_auth_session_context_t *session,
                                             const char *owner,
                                             uint32_t expected_lease_epoch)
{
    (void)hid_actions_release_all(session, owner, expected_lease_epoch);
    si_hid_json_add_action_result(results, index, ESP_ERR_INVALID_STATE,
                                  reason);
    cJSON_AddBoolToObject(response, "aborted", true);
    cJSON_AddStringToObject(response, "abort_reason",
                            reason ? reason : "HID control revoked");
}

esp_err_t control_lease_handler(httpd_req_t *req)
{
    si_auth_session_context_t session;
    si_capability_t required =
        req->method == HTTP_GET ? SI_CAPABILITY_OBSERVE :
                                  SI_CAPABILITY_CONTROL_LEASE;
    esp_err_t auth_ret =
        si_http_require_capability(req, required, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        add_current_control_lease_json(resp);
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char buf[512] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }

    const char *owner = control_owner_for_session(&session);
    if (!owner) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "session cannot own control lease");
    }
    const char *claimed_owner =
        device_json_string_any(root, "owner", "source", NULL);
    if (claimed_owner && strcasecmp(claimed_owner, owner) != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "lease owner is derived from session");
    }

    cJSON *active_item = cJSON_GetObjectItemCaseSensitive(root, "active");
    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *force_item = cJSON_GetObjectItemCaseSensitive(root, "force");
    cJSON *takeover_item = cJSON_GetObjectItemCaseSensitive(root, "takeover");
    cJSON *reclaim_item = cJSON_GetObjectItemCaseSensitive(root, "reclaim");
    bool active = cJSON_IsBool(active_item) ? cJSON_IsTrue(active_item) :
                  (!cJSON_IsBool(enabled_item) || cJSON_IsTrue(enabled_item));
    bool force = (cJSON_IsBool(force_item) && cJSON_IsTrue(force_item)) ||
                 (cJSON_IsBool(takeover_item) && cJSON_IsTrue(takeover_item)) ||
                 (cJSON_IsBool(reclaim_item) && cJSON_IsTrue(reclaim_item));
    if (force && session.principal != SI_PRINCIPAL_BROWSER) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "only browser sessions may force takeover");
    }
    const char *mode = device_json_string_any(root, "mode", "permission", "control_mode");
    if (!mode) {
        mode = "supervised";
    }
    const char *reason = device_json_string_any(root, "reason", "message", NULL);

    if (active && !si_control_lease_mode_valid(mode)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be observe, supervised, or autonomous");
    }
    si_control_lease_update_result_t update_result =
        si_control_lease_update_for_auth_session(
            owner, session.session_id, session.generation, mode, reason,
            active, force, control_lease_auth_live, &session);
    cJSON_Delete(root);

    if (update_result == SI_CONTROL_LEASE_UPDATE_HELD_BY_OTHER) {
        return si_http_send_text_status(req, "409 Conflict", "control lease held by another owner");
    }
    if (update_result == SI_CONTROL_LEASE_UPDATE_UNAVAILABLE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "control lease unavailable");
    }
    if (update_result != SI_CONTROL_LEASE_UPDATE_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid control lease request");
    }

    cJSON *resp = cJSON_CreateObject();
    add_current_control_lease_json(resp);
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

esp_err_t hid_actions_handler(httpd_req_t *req)
{
    si_auth_session_context_t session;
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_HID, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (!direct_hid_principal_allowed(session.principal)) {
        return httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN,
            "direct HID actions require a browser or external MCP session; "
            "the embedded Agent must use the Request Broker/Execution Gateway");
    }

    const char *owner = control_owner_for_session(&session);
    si_control_lease_status_t lease;
    si_control_lease_get_status(&lease);
    si_authorization_request_t authorization = {
        .required_capability = SI_CAPABILITY_HID,
        .policy_allowed = true,
        .lease_required = true,
        .lease_held = owner &&
            si_control_lease_owner_session_matches(
                owner, session.session_id),
        .human_kvm_active = lease.kvm_active,
        .mode = control_authorization_mode(lease.mode),
        .risk = SI_AUTHZ_RISK_HIGH,
    };
    auth_ret =
        si_http_require_authorization(req, &authorization, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (!owner) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "session cannot issue HID actions");
    }
    const uint32_t expected_lease_epoch = lease.epoch;
    si_hid_owner_token_t hid_owner = {0};
    if (!si_control_lease_get_hid_owner(
            owner, session.session_id, session.generation,
            expected_lease_epoch, &hid_owner)) {
        return si_http_send_text_status(
            req, "409 Conflict", "HID report owner changed");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = device_http_calloc(1, HID_ACTIONS_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, HID_ACTIONS_MAX_BODY, &root);
    free(buf);
    if (json_ret != ESP_OK) {
        return json_ret;
    }

    const char *claimed_owner =
        device_json_string_any(root, "owner", "source", NULL);
    if (claimed_owner && strcasecmp(claimed_owner, owner) != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "HID owner is derived from session");
    }
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");
    if (!cJSON_IsArray(actions)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "actions array required");
    }
    int count = cJSON_GetArraySize(actions);
    if (count < 1 || count > HID_ACTIONS_MAX_ITEMS) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "actions count out of range");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "owner", owner);
    cJSON *results = cJSON_AddArrayToObject(resp, "results");
    bool all_ok = true;
    int executed = 0;
    for (int i = 0; i < count; i++) {
        const char *control_error = NULL;
        if (!hid_actions_control_checkpoint(
                &session, owner, expected_lease_epoch, &control_error)) {
            hid_actions_record_control_abort(resp, results, i,
                                             control_error, &session, owner,
                                             expected_lease_epoch);
            all_ok = false;
            executed++;
            break;
        }

        cJSON *action = cJSON_GetArrayItem(actions, i);
        if (!cJSON_IsObject(action)) {
            (void)hid_actions_release_all(
                &session, owner, expected_lease_epoch);
            si_hid_json_add_action_result(results, i, ESP_ERR_INVALID_ARG,
                                          "action must be object");
            all_ok = false;
            executed++;
            break;
        }

        cJSON *type = cJSON_GetObjectItemCaseSensitive(action, "type");
        esp_err_t action_ret = ESP_OK;
        if (cJSON_IsString(type) && type->valuestring &&
            strcasecmp(type->valuestring, "wait") == 0) {
            uint32_t wait_ms = si_hid_json_action_wait_ms(action);
            if (!hid_actions_guarded_wait(
                    &session, owner, expected_lease_epoch, wait_ms,
                    &control_error)) {
                hid_actions_record_control_abort(resp, results, i,
                                                 control_error, &session,
                                                 owner,
                                                 expected_lease_epoch);
                all_ok = false;
                executed++;
                break;
            }
        } else {
            hid_actions_authority_guard_t guard = {
                .session = &session,
                .owner = owner,
                .expected_lease_epoch = expected_lease_epoch,
            };
            action_ret = si_hid_json_execute_owned(
                action, &hid_owner, hid_actions_authority_guard, &guard);
            if (action_ret == ESP_ERR_INVALID_STATE) {
                control_error = guard.denial_reason;
                if (!control_error) {
                    (void)hid_actions_control_checkpoint(
                        &session, owner, expected_lease_epoch,
                        &control_error);
                }
                if (control_error) {
                    hid_actions_record_control_abort(
                        resp, results, i, control_error, &session, owner,
                        expected_lease_epoch);
                    all_ok = false;
                    executed++;
                    break;
                }
            }
        }

        si_hid_json_add_action_result(results, i, action_ret, NULL);
        executed++;
        if (action_ret != ESP_OK) {
            (void)hid_actions_release_all(
                &session, owner, expected_lease_epoch);
            all_ok = false;
            break;
        }
    }
    cJSON_AddBoolToObject(resp, "ok", all_ok);
    cJSON_AddNumberToObject(resp, "executed", executed);

    cJSON_Delete(root);
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}
