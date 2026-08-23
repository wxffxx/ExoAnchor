// Device status serialization and HTTP adapters.
#include "device_http.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "control_lease.h"
#include "device_observation_utils.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_json.h"
#include "http_api.h"
#include "web_json.h"

#define HID_ACTIONS_MAX_BODY 4096
#define HID_ACTIONS_MAX_ITEMS 64

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
    cJSON_AddStringToObject(flash, "usage_basis", "partition_map_reserved");
    cJSON_AddBoolToObject(flash, "detected", info->detected);
    cJSON_AddNumberToObject(flash, "total_bytes", info->total_bytes);
    cJSON_AddNumberToObject(flash, "used_bytes", info->reserved_bytes);
    cJSON_AddNumberToObject(flash, "free_bytes", info->free_bytes);
    cJSON_AddNumberToObject(flash, "partition_bytes", info->partition_bytes);
    cJSON_AddNumberToObject(flash, "app_partition_bytes", info->app_partition_bytes);
    cJSON_AddNumberToObject(flash, "data_partition_bytes", info->data_partition_bytes);
    cJSON_AddNumberToObject(flash, "partition_count", info->partition_count);
    cJSON_AddNumberToObject(flash, "usage_percent",
                            si_observation_used_percent(info->total_bytes,
                                                        info->free_bytes));
    cJSON_AddStringToObject(flash, "running_partition", info->running_partition);
    cJSON_AddStringToObject(flash, "boot_partition", info->boot_partition);
    cJSON_AddNumberToObject(flash, "running_partition_bytes",
                            info->running_partition_bytes);
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

esp_err_t system_info_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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
    cJSON_AddNumberToObject(disk, "used_gb", status.flash.reserved_bytes / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "free_gb", status.flash.free_bytes / 1024.0 / 1024.0 / 1024.0);
    cJSON_AddNumberToObject(disk, "usage_percent",
                            si_observation_used_percent(status.flash.total_bytes,
                                                        status.flash.free_bytes));
    add_flash_storage_json(disk, "flash", &status.flash);
    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON *eth = cJSON_AddObjectToObject(network, "ethernet");
    cJSON_AddBoolToObject(eth, "up", status.network.connected);
    cJSON_AddBoolToObject(eth, "link_up", status.network.link_up);
    cJSON_AddStringToObject(eth, "phy", status.network.phy);
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

    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t logs_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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
    si_device_log_observation_t *snapshot = calloc((size_t)count, sizeof(*snapshot));
    size_t total = snapshot ?
                   si_device_observation_log_snapshot(snapshot, (size_t)count) : 0;
    for (size_t i = 0; i < total; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "time", snapshot[i].time);
        cJSON_AddStringToObject(entry, "level", snapshot[i].level);
        cJSON_AddStringToObject(entry, "message", snapshot[i].message);
        cJSON_AddItemToArray(logs, entry);
    }
    free(snapshot);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t logs_download_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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

    si_device_log_observation_t *snapshot = calloc((size_t)count, sizeof(*snapshot));
    size_t total = snapshot ?
                   si_device_observation_log_snapshot(snapshot, (size_t)count) : 0;
    for (size_t i = 0; i < total; i++) {
        n = snprintf(line, sizeof(line), "%s %-5s %s\n",
                     snapshot[i].time, snapshot[i].level, snapshot[i].message);
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
    cJSON_AddBoolToObject(root, "enabled", video->enabled);
    cJSON_AddBoolToObject(root, "connected", video->frame_ready);
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
    cJSON_AddNumberToObject(root, "last_frame_ms", video->last_frame_ms);
    cJSON_AddNumberToObject(root, "fps", video->fps_x100 / 100.0);
    cJSON_AddStringToObject(root, "last_error", video->last_error);
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
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_video_observation_t video;
    si_device_video_control_observation_t control;
    si_device_video_mode_observation_t *modes =
        calloc(SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES, sizeof(*modes));
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
    cJSON_AddBoolToObject(keyboard, "available", hid->ready);
    cJSON *mouse = cJSON_AddObjectToObject(root, "mouse");
    cJSON_AddBoolToObject(mouse, "available", hid->ready);

    cJSON *stats = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(stats, "tx_messages", hid->tx_messages);
    cJSON_AddNumberToObject(stats, "failed_messages", hid->failed_messages);
    cJSON_AddStringToObject(root, "last_error", hid->last_error);
}

esp_err_t hid_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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
    cJSON_AddBoolToObject(locator, "supported", power_status->locator_supported);
    cJSON_AddBoolToObject(locator, "active", power_status->locator_on);
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

void si_device_http_add_network_json(cJSON *root,
                                     const si_device_network_observation_t *network)
{
    cJSON_AddBoolToObject(root, "configured", network->configured);
    cJSON_AddBoolToObject(root, "connected", network->connected);
    cJSON_AddBoolToObject(root, "link_up", network->link_up);
    cJSON_AddBoolToObject(root, "full_duplex", network->full_duplex);
    cJSON_AddNumberToObject(root, "speed_mbps", network->speed_mbps);
    cJSON_AddStringToObject(root, "interface", network->interface);
    cJSON_AddStringToObject(root, "driver", network->driver);
    cJSON_AddStringToObject(root, "phy", network->phy);
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
    cJSON_AddStringToObject(storage, "schema_version", "exoanchor.flash.v1");
    add_flash_storage_json(storage, "flash", &performance->flash);
    cJSON *storage_devices = cJSON_AddArrayToObject(storage, "devices");
    add_storage_device_summary(storage_devices, "internal_flash", "Flash",
                               "nor_flash", "internal_spi", "firmware", true,
                               performance->flash.detected, false,
                               performance->flash.total_bytes,
                               performance->flash.reserved_bytes,
                               performance->flash.free_bytes,
                               si_observation_used_percent(performance->flash.total_bytes,
                                                           performance->flash.free_bytes),
                               "");

    cJSON *stream = cJSON_AddObjectToObject(root, "video");
    cJSON_AddNumberToObject(stream, "fps", fps);
    cJSON_AddNumberToObject(stream, "target_fps", target_fps);
    cJSON_AddNumberToObject(stream, "estimated_mbps", estimated_mbps);
    cJSON_AddNumberToObject(stream, "last_jpeg_size", video->last_jpeg_size);
    cJSON_AddNumberToObject(stream, "frame_interval_ms", video->last_frame_ms);
    cJSON_AddNumberToObject(stream, "expected_frame_interval_ms",
                            target_fps > 0.0 ? 1000.0 / target_fps : 0.0);
    cJSON_AddNumberToObject(stream, "frames_captured", video->frames_captured);
    cJSON_AddNumberToObject(stream, "frames_encoded", video->frames_encoded);
    cJSON_AddNumberToObject(stream, "frames_dropped", video->frames_dropped);
    cJSON_AddNumberToObject(stream, "drop_percent", drop_percent);
}

esp_err_t capabilities_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "family", "ESP32P4");
    cJSON_AddStringToObject(device, "board", SI_BOARD_ID);
    cJSON_AddStringToObject(device, "board_name", SI_BOARD_NAME);
    cJSON_AddStringToObject(device, "silicon_target", SI_SILICON_TARGET);
    cJSON_AddStringToObject(device, "label", system.device_label);
    cJSON_AddStringToObject(device, "hostname", system.hostname);
    cJSON_AddStringToObject(device, "firmware_version", app ? app->version : "");

    cJSON *kvm_core = cJSON_AddObjectToObject(root, "kvm_core");
    cJSON_AddStringToObject(kvm_core, "edition", "stable-kvm");
    cJSON_AddBoolToObject(kvm_core, "manual_ui", true);
    cJSON_AddBoolToObject(kvm_core, "embedded_agent", false);
    cJSON_AddBoolToObject(kvm_core, "ssh_client", false);
    cJSON_AddBoolToObject(kvm_core, "tf_file_manager", false);
    cJSON_AddBoolToObject(kvm_core, "external_control_api", true);
    cJSON_AddBoolToObject(kvm_core, "control_lease", true);
    cJSON_AddNumberToObject(kvm_core, "control_lease_ms",
                            SI_CONTROL_LEASE_GRACE_MS);
    cJSON_AddBoolToObject(kvm_core, "human_kvm_preempts_external_control", true);

    cJSON *settings_caps = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddBoolToObject(settings_caps, "nvs", true);
    cJSON_AddBoolToObject(settings_caps, "session", true);
    cJSON_AddBoolToObject(settings_caps, "video", true);
    cJSON_AddBoolToObject(settings_caps, "ota", false);

    cJSON *video_caps = cJSON_AddObjectToObject(root, "video");
    cJSON_AddBoolToObject(video_caps, "enabled", video.enabled);
    cJSON_AddBoolToObject(video_caps, "initialized", video.initialized);
    cJSON_AddBoolToObject(video_caps, "connected", video.frame_ready);
    cJSON_AddBoolToObject(video_caps, "snapshot", true);
    cJSON_AddBoolToObject(video_caps, "mjpeg_stream", true);
    cJSON_AddBoolToObject(video_caps, "lease", true);
    cJSON_AddBoolToObject(video_caps, "external_control_lease", true);
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

    cJSON *uart_caps = cJSON_AddObjectToObject(root, "uart");
    cJSON_AddBoolToObject(uart_caps, "supported", false);
    cJSON_AddStringToObject(uart_caps, "note", "Target-host UART is not implemented in Stable KVM.");

    cJSON *observations = cJSON_AddObjectToObject(root, "observations");
    cJSON_AddStringToObject(observations, "device_status", "/api/status");
    cJSON_AddStringToObject(observations, "power_status", "/api/power/status");
    cJSON_AddStringToObject(observations, "hid_status", "/api/hid/status");
    cJSON_AddStringToObject(observations, "snapshot", "/api/snapshot");
    cJSON_AddStringToObject(observations, "control_lease", "/api/control/lease");

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
    cJSON_AddBoolToObject(root, "kvm_active", lease->kvm_active);
    cJSON_AddNumberToObject(root, "kvm_expires_in_ms", lease->kvm_remaining_ms);
    cJSON_AddBoolToObject(root, "agent_active", lease->agent_active);
    cJSON_AddStringToObject(root, "agent_owner", lease->agent_owner);
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

esp_err_t control_lease_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_auth(req);
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

    const char *owner = device_json_string_any(root, "owner", "source", NULL);
    if (!si_control_lease_owner_valid(owner)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "owner must be agent or mcp");
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
        si_control_lease_update(owner, mode, reason, active, force);
    cJSON_Delete(root);

    if (update_result == SI_CONTROL_LEASE_UPDATE_KVM_ACTIVE) {
        return si_http_send_text_status(req, "409 Conflict", "kvm control is active");
    }
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
    esp_err_t auth_ret = si_http_require_auth(req);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (!si_control_lease_agent_active()) {
        return si_http_send_text_status(req, "409 Conflict", "agent control lease required");
    }
    if (!si_control_lease_agent_allows_actions()) {
        return si_http_send_text_status(req, "409 Conflict", "agent control lease does not allow actions");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body required");
    }

    char *buf = calloc(1, HID_ACTIONS_MAX_BODY);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, HID_ACTIONS_MAX_BODY, &root);
    free(buf);
    if (json_ret != ESP_OK) {
        return json_ret;
    }

    const char *owner = device_json_string_any(root, "owner", "source", NULL);
    if (!si_control_lease_owner_valid(owner)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "owner must be agent or mcp");
    }
    bool owner_matches = si_control_lease_owner_matches(owner);
    if (!owner_matches) {
        cJSON_Delete(root);
        return si_http_send_text_status(req, "409 Conflict", "control lease owner mismatch");
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
        cJSON *action = cJSON_GetArrayItem(actions, i);
        if (!cJSON_IsObject(action)) {
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
            if (wait_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
            }
        } else {
            action_ret = si_hid_json_execute(action);
        }

        si_hid_json_add_action_result(results, i, action_ret, NULL);
        executed++;
        if (action_ret != ESP_OK) {
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
