#include "device_observation_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "auth_service.h"
#include "control_lease.h"
#include "device_observation_utils.h"
#include "device_settings.h"
#include "esp_chip_info.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "net_manager.h"
#include "nvs.h"
#include "power_control.h"
#include "storage_manager.h"
#include "time_utils.h"
#include "utf8_utils.h"
#include "video_control.h"
#include "video_input.h"
#include "video_stream_metrics.h"

#if SI_CFG_MS2109_POWER_ENABLED
#include "ms2109_power.h"
#endif
#if SI_CFG_MS2109_TEST_ENABLED
#include "ms2109_test.h"
#endif

_Static_assert(SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES >= SI_VIDEO_MAX_MODES,
               "observation DTO must hold every video mode");
_Static_assert(SI_DEVICE_OBSERVATION_MAX_POWER_GPIO >= SI_POWER_GPIO_MAP_MAX,
               "observation DTO must hold the complete power GPIO map");

static EXT_RAM_BSS_ATTR si_device_log_observation_t
    s_logs[SI_DEVICE_OBSERVATION_MAX_LOGS];
static size_t s_log_head;
static size_t s_log_count;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_cpu_usage_valid;
static int64_t s_cpu_prev_wall_us;
static configRUN_TIME_COUNTER_TYPE s_cpu_prev_idle;

static void *observation_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : calloc(count, size);
}

uint32_t si_device_observation_uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void si_device_observation_get_network(si_device_network_observation_t *out)
{
    if (!out) {
        return;
    }
    si_net_status_t status;
    si_net_get_status(&status);
    memset(out, 0, sizeof(*out));
    out->configured = status.configured;
    out->connected = status.connected;
    out->link_up = status.link_up;
    out->full_duplex = status.full_duplex;
    out->retry_count = status.retry_count;
    out->speed_mbps = status.speed_mbps;
    out->pending_confirmation = status.pending_confirmation;
    out->recovery_active = status.recovery_active;
    out->config_generation = status.config_generation;
    out->confirm_remaining_seconds = status.confirm_remaining_seconds;
    strlcpy(out->interface, status.interface, sizeof(out->interface));
    strlcpy(out->driver, status.driver, sizeof(out->driver));
    strlcpy(out->device_id, status.device_id, sizeof(out->device_id));
    strlcpy(out->hostname, status.hostname, sizeof(out->hostname));
    strlcpy(out->mode, status.mode, sizeof(out->mode));
    strlcpy(out->address_source, status.address_source,
            sizeof(out->address_source));
    strlcpy(out->config_state, status.config_state,
            sizeof(out->config_state));
    strlcpy(out->ip, status.ip, sizeof(out->ip));
    strlcpy(out->netmask, status.netmask, sizeof(out->netmask));
    strlcpy(out->gateway, status.gateway, sizeof(out->gateway));
    strlcpy(out->mac, status.mac, sizeof(out->mac));
}

static void collect_video_control(si_device_video_control_observation_t *out)
{
    if (!out) {
        return;
    }
    si_video_control_status_t status;
    si_video_control_get_status(&status);
    memset(out, 0, sizeof(*out));
    out->preview_enabled = status.preview_enabled;
    out->always_on = status.always_on;
    out->suspended = status.suspended;
    out->kvm_active = status.kvm_active;
    out->agent_active = status.agent_active;
    out->agent_takeover = status.agent_takeover;
    out->kvm_remaining_ms = status.kvm_remaining_ms;
    out->agent_remaining_ms = status.agent_remaining_ms;
    out->kvm_width = status.kvm_width;
    out->kvm_height = status.kvm_height;
    out->kvm_fps_x100 = status.kvm_fps_x100;
    out->active_enabled = status.active_enabled;
    out->active_width = status.active_width;
    out->active_height = status.active_height;
    out->active_fps_x100 = status.active_fps_x100;
    out->active_stride_ms = status.active_stride_ms;
    strlcpy(out->active_owner, status.active_owner, sizeof(out->active_owner));
    out->kvm_grace_ms = SI_VIDEO_CONTROL_KVM_GRACE_MS;
    out->agent_grace_ms = SI_VIDEO_CONTROL_AGENT_GRACE_MS;
    out->preview_width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
    out->preview_height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
    out->preview_fps_x100 = status.preview_fps_x100;
    out->preview_stride_ms = status.preview_stride_ms;
}

void si_device_observation_get_video(si_device_video_observation_t *out,
                                     si_device_video_control_observation_t *control_out,
                                     si_device_video_mode_observation_t *modes,
                                     size_t max_modes, size_t *mode_count_out)
{
    if (mode_count_out) {
        *mode_count_out = 0;
    }
    if (out) {
        si_video_status_t status;
        si_video_get_status(&status);
        memset(out, 0, sizeof(*out));
        out->enabled = status.enabled;
        out->initialized = status.initialized;
        out->streaming = status.streaming;
        out->frame_ready = status.frame_ready;
        out->capture_enabled = status.capture_enabled;
        out->width = status.width;
        out->height = status.height;
        out->target_width = status.target_width;
        out->target_height = status.target_height;
        out->target_fps_x100 = status.target_fps_x100;
        out->capture_stride_ms = status.capture_stride_ms;
        out->modes_count = status.modes_count;
        out->data_lanes = status.data_lanes;
        out->lane_bitrate_mbps = status.lane_bitrate_mbps;
        out->jpeg_quality = status.jpeg_quality;
        out->frames_captured = status.frames_captured;
        out->frames_encoded = status.frames_encoded;
        out->frames_dropped = status.frames_dropped;
        out->last_jpeg_size = status.last_jpeg_size;
        out->frame_interval_ms = status.frame_interval_ms;
        out->last_frame_ms = status.last_frame_ms;
        out->fps_x100 = status.fps_x100;
        out->uvc_callbacks = status.uvc_callbacks;
        out->uvc_queue_drops = status.uvc_queue_drops;
        out->h264_pressure_coalesces = status.h264_pressure_coalesces;
        out->uvc_buffer_underflows = status.uvc_buffer_underflows;
        out->uvc_buffer_overflows = status.uvc_buffer_overflows;
        out->uvc_return_pending = status.uvc_return_pending;
        out->uvc_return_retries = status.uvc_return_retries;
        out->uvc_return_failures = status.uvc_return_failures;
        out->ingest_validate_last_us = status.ingest_validate_last_us;
        out->ingest_validate_max_us = status.ingest_validate_max_us;
        out->ingest_publish_last_us = status.ingest_publish_last_us;
        out->ingest_publish_max_us = status.ingest_publish_max_us;
        out->ingest_total_last_us = status.ingest_total_last_us;
        out->ingest_total_max_us = status.ingest_total_max_us;
        si_video_stream_metrics_t stream_metrics;
        si_video_stream_metrics_get(&stream_metrics, si_monotonic_ms());
        out->stream_metrics_valid = stream_metrics.valid;
        out->stream_metrics_stream_id = stream_metrics.stream_id;
        out->stream_fps_x100 = stream_metrics.fps_x100;
        out->stream_bitrate_bps = stream_metrics.bitrate_bps;
        out->stream_sample_window_ms = stream_metrics.sample_window_ms;
        strlcpy(out->source, status.source, sizeof(out->source));
        strlcpy(out->pixel_format, status.pixel_format, sizeof(out->pixel_format));
        strlcpy(out->capture_owner, status.capture_owner, sizeof(out->capture_owner));
        strlcpy(out->last_error, status.last_error, sizeof(out->last_error));
    }
    collect_video_control(control_out);
    if (!modes || max_modes == 0) {
        return;
    }
    if (max_modes > SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES) {
        max_modes = SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES;
    }
    si_video_mode_t *raw_modes =
        observation_calloc(max_modes, sizeof(*raw_modes));
    if (!raw_modes) {
        return;
    }
    size_t raw_count = 0;
    if (si_video_get_modes(raw_modes, max_modes, &raw_count) != ESP_OK) {
        free(raw_modes);
        return;
    }
    size_t count = raw_count < max_modes ? raw_count : max_modes;
    for (size_t i = 0; i < count; i++) {
        modes[i].selected = raw_modes[i].selected;
        modes[i].width = raw_modes[i].width;
        modes[i].height = raw_modes[i].height;
        modes[i].fps_x100 = raw_modes[i].fps_x100;
        strlcpy(modes[i].pixel_format, raw_modes[i].pixel_format,
                sizeof(modes[i].pixel_format));
    }
    if (mode_count_out) {
        *mode_count_out = count;
    }
    free(raw_modes);
}

void si_device_observation_get_hid(si_device_hid_observation_t *out)
{
    if (!out) {
        return;
    }
    si_hid_status_t status;
    si_hid_get_status(&status);
    memset(out, 0, sizeof(*out));
    out->enabled = status.enabled;
    out->initialized = status.initialized;
    out->mounted = status.mounted;
    out->ready = status.ready;
    out->keyboard_writable = status.keyboard_writable;
    out->mouse_writable = status.mouse_writable;
    out->absolute_pointer_writable = status.absolute_pointer_writable;
    out->dm_gpio = status.dm_gpio;
    out->dp_gpio = status.dp_gpio;
    out->tx_messages = status.tx_messages;
    out->failed_messages = status.failed_messages;
    strlcpy(out->mode, status.mode, sizeof(out->mode));
    strlcpy(out->port, status.port, sizeof(out->port));
    strlcpy(out->transport, status.transport, sizeof(out->transport));
    strlcpy(out->last_error, status.last_error, sizeof(out->last_error));
}

void si_device_observation_get_power(si_device_power_observation_t *out)
{
    if (!out) {
        return;
    }
    si_power_status_t status;
    si_power_get_status(&status);
    memset(out, 0, sizeof(*out));
    out->enabled = status.enabled;
    out->initialized = status.initialized;
    out->busy = status.busy;
    out->active_high = status.active_high;
    out->power_detect_supported = status.power_detect_supported;
    out->standby_detect_supported = status.standby_detect_supported;
    out->power_on = status.power_on;
    out->standby_on = status.standby_on;
    out->locator_supported = status.locator_supported;
    out->locator_on = status.locator_on;
    out->locator_bidirectional = status.locator_bidirectional;
    out->locator_reversed = status.locator_reversed;
    out->power_button_gpio = status.power_button_gpio;
    out->reset_button_gpio = status.reset_button_gpio;
    out->power_detect_gpio = status.power_detect_gpio;
    out->standby_detect_gpio = status.standby_detect_gpio;
    out->locator_gpio = status.locator_gpio;
    out->locator_return_gpio = status.locator_return_gpio;
    out->power_detect_active_high = status.power_detect_active_high;
    out->standby_detect_active_high = status.standby_detect_active_high;
    out->locator_active_high = status.locator_active_high;
    out->default_press_ms = status.default_press_ms;
    out->force_off_ms = status.force_off_ms;
    out->action_count = status.action_count;
    strlcpy(out->last_action, status.last_action, sizeof(out->last_action));
    strlcpy(out->last_error, status.last_error, sizeof(out->last_error));
    out->gpio_map_count = status.gpio_map_count < SI_DEVICE_OBSERVATION_MAX_POWER_GPIO ?
                          status.gpio_map_count : SI_DEVICE_OBSERVATION_MAX_POWER_GPIO;
    for (size_t i = 0; i < out->gpio_map_count; i++) {
        const si_power_gpio_map_entry_t *src = &status.gpio_map[i];
        si_device_power_gpio_observation_t *dst = &out->gpio_map[i];
        strlcpy(dst->role, src->role, sizeof(dst->role));
        strlcpy(dst->label, src->label, sizeof(dst->label));
        strlcpy(dst->direction, src->direction, sizeof(dst->direction));
        dst->gpio = src->gpio;
        dst->enabled = src->enabled;
        dst->implemented = src->implemented;
        dst->active_high = src->active_high;
        dst->active = src->active;
        dst->configurable = src->configurable;
        dst->required = src->required;
        strlcpy(dst->state, src->state, sizeof(dst->state));
    }
}

void si_device_observation_get_ms2109_power(
    si_device_ms2109_power_observation_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->state_kind, "commanded", sizeof(out->state_kind));
    out->rail_feedback_available = false;
#if SI_CFG_MS2109_TEST_ENABLED
    si_ms2109_test_status_t status;
    si_ms2109_test_get_status(&status);
    out->supported = status.enabled;
    out->initialized = status.initialized;
    out->power_on = status.power_on;
    out->switch_gpio = status.switch_gpio;
    out->core_enable_gpio = status.core_enable_gpio;
    out->switch_output_level = status.switch_output_level;
    out->core_enable_output_level = status.core_enable_output_level;
    out->operation_count = status.operation_count;
    strlcpy(out->last_result, esp_err_to_name(status.last_result),
            sizeof(out->last_result));
#elif SI_CFG_MS2109_POWER_ENABLED
    si_ms2109_power_status_t status;
    esp_err_t result = si_ms2109_power_get_status(&status);
    if (result != ESP_OK) {
        strlcpy(out->last_result, esp_err_to_name(result),
                sizeof(out->last_result));
        return;
    }
    out->supported = status.enabled;
    out->initialized = status.initialized;
    out->power_on = status.power_on;
    out->switch_gpio = status.switch_gpio;
    out->core_enable_gpio = status.core_enable_gpio;
    out->switch_output_level = status.switch_output_level;
    out->core_enable_output_level = status.core_enable_output_level;
    out->operation_count = status.operation_count;
    strlcpy(out->last_result, esp_err_to_name(status.last_result),
            sizeof(out->last_result));
#else
    strlcpy(out->last_result, "ESP_ERR_NOT_SUPPORTED",
            sizeof(out->last_result));
#endif
}

static void collect_flash(si_device_flash_observation_t *out)
{
    memset(out, 0, sizeof(*out));
    out->detected = esp_flash_get_size(NULL, &out->total_bytes) == ESP_OK &&
                    out->total_bytes > 0;
    esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                            ESP_PARTITION_SUBTYPE_ANY,
                                                            NULL);
    for (; iterator != NULL; iterator = esp_partition_next(iterator)) {
        const esp_partition_t *part = esp_partition_get(iterator);
        if (!part) {
            continue;
        }
        out->partition_count++;
        out->partition_bytes += part->size;
        uint32_t end = part->address + part->size;
        if (end > out->reserved_bytes) {
            out->reserved_bytes = end;
        }
        if (part->type == ESP_PARTITION_TYPE_APP) {
            out->app_partition_bytes += part->size;
        } else if (part->type == ESP_PARTITION_TYPE_DATA) {
            out->data_partition_bytes += part->size;
        }
        if (strcmp(part->label, "nvs") == 0) {
            out->nvs_partition_bytes = part->size;
        }
    }
    esp_partition_iterator_release(iterator);
    if (out->reserved_bytes > out->total_bytes) {
        out->reserved_bytes = out->total_bytes;
    }
    if (out->partition_bytes > out->total_bytes) {
        out->partition_bytes = out->total_bytes;
    }
    out->layout_gap_bytes = out->reserved_bytes > out->partition_bytes ?
                            out->reserved_bytes - out->partition_bytes : 0;
    out->free_bytes = out->total_bytes - out->partition_bytes;

    nvs_stats_t nvs_stats = {0};
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        out->nvs_stats_available = true;
        out->nvs_total_entries = (uint32_t)nvs_stats.total_entries;
        out->nvs_used_entries = (uint32_t)nvs_stats.used_entries;
        out->nvs_free_entries = (uint32_t)nvs_stats.free_entries;
        out->nvs_namespace_count = (uint32_t)nvs_stats.namespace_count;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    strlcpy(out->running_partition, running ? running->label : "--",
            sizeof(out->running_partition));
    strlcpy(out->boot_partition, boot ? boot->label : "--",
            sizeof(out->boot_partition));
    strlcpy(out->next_update_partition, next ? next->label : "--",
            sizeof(out->next_update_partition));
    out->running_partition_bytes = running ? running->size : 0;
    out->next_update_partition_bytes = next ? next->size : 0;
}

static void collect_tf(si_device_tf_observation_t *out)
{
    si_storage_tf_status_t status;
    si_storage_get_tf_status(&status);
    memset(out, 0, sizeof(*out));
    out->supported = status.supported;
    out->mounted = status.mounted;
    out->degraded_mode = status.degraded_mode;
    out->total_bytes = status.total_bytes;
    out->used_bytes = status.used_bytes;
    out->free_bytes = status.free_bytes;
    out->usage_percent = status.usage_percent;
    out->sector_size = status.sector_size;
    out->cluster_size = status.cluster_size;
    out->bus_frequency_khz = status.bus_frequency_khz;
    out->bus_width = status.bus_width;
    strlcpy(out->mount_point, status.mount_point, sizeof(out->mount_point));
    strlcpy(out->card_name, status.card_name, sizeof(out->card_name));
    strlcpy(out->card_type, status.card_type, sizeof(out->card_type));
    strlcpy(out->bus_mode, status.bus_mode, sizeof(out->bus_mode));
    strlcpy(out->fallback_reason, status.fallback_reason, sizeof(out->fallback_reason));
    strlcpy(out->last_error, status.last_error, sizeof(out->last_error));
}

void si_device_observation_get_storage(si_device_flash_observation_t *flash_out,
                                       si_device_tf_observation_t *tf_out)
{
    if (flash_out) {
        collect_flash(flash_out);
    }
    if (tf_out) {
        collect_tf(tf_out);
    }
}

void si_device_observation_get_control_lease(si_device_control_lease_observation_t *out)
{
    if (!out) {
        return;
    }
    si_control_lease_status_t status;
    si_control_lease_get_status(&status);
    memset(out, 0, sizeof(*out));
    out->active = status.active;
    out->kvm_view_active = status.kvm_view_active;
    out->kvm_active = status.kvm_active;
    out->agent_active = status.agent_active;
    out->input_control_active = status.input_control_active;
    out->agent_takeover = status.agent_takeover;
    out->can_request = status.can_request;
    out->kvm_remaining_ms = status.kvm_remaining_ms;
    out->agent_remaining_ms = status.agent_remaining_ms;
    out->expires_in_ms = status.expires_in_ms;
    out->lease_ms = SI_CONTROL_LEASE_GRACE_MS;
    strlcpy(out->owner, status.owner, sizeof(out->owner));
    strlcpy(out->mode, status.mode, sizeof(out->mode));
    strlcpy(out->agent_owner, status.agent_owner, sizeof(out->agent_owner));
    strlcpy(out->session_id, status.session_id, sizeof(out->session_id));
    strlcpy(out->reason, status.reason, sizeof(out->reason));
}

void si_device_observation_get_system(si_device_system_observation_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    out->cpu_cores = chip_info.cores;
    out->free_heap_bytes = esp_get_free_heap_size();
    out->minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
    out->uptime_seconds = si_device_observation_uptime_seconds();
    si_observation_format_uptime(out->uptime_seconds, out->uptime_formatted,
                                 sizeof(out->uptime_formatted));
    out->login_count = si_auth_login_count();
    si_device_label_get(out->device_label, sizeof(out->device_label));
    si_device_observation_get_network(&out->network);
    strlcpy(out->hostname,
            out->network.hostname[0] ? out->network.hostname
                                     : SI_BMC_HOSTNAME,
            sizeof(out->hostname));
    si_device_observation_get_storage(&out->flash, &out->tf);
}

static void collect_heap(si_device_heap_observation_t *out, uint32_t caps)
{
    memset(out, 0, sizeof(*out));
    out->total_bytes = heap_caps_get_total_size(caps);
    out->free_bytes = heap_caps_get_free_size(caps);
    out->minimum_free_bytes = heap_caps_get_minimum_free_size(caps);
    out->largest_free_block = heap_caps_get_largest_free_block(caps);
    out->used_bytes = out->total_bytes > out->free_bytes ?
                      out->total_bytes - out->free_bytes : 0;
}

static double clamp_percent(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    return value > 100.0 ? 100.0 : value;
}

static void collect_cpu(si_device_cpu_observation_t *out)
{
    memset(out, 0, sizeof(*out));
    out->core_count = CONFIG_FREERTOS_NUMBER_OF_CORES;
#if (configGENERATE_RUN_TIME_STATS == 1) && (INCLUDE_xTaskGetIdleTaskHandle == 1)
    int64_t wall_us = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE idle_runtime = ulTaskGetIdleRunTimeCounter();
    uint64_t wall_delta = s_cpu_usage_valid && wall_us > s_cpu_prev_wall_us ?
                          (uint64_t)(wall_us - s_cpu_prev_wall_us) : 0;
    uint64_t idle_delta = s_cpu_usage_valid ?
                          (uint64_t)(idle_runtime - s_cpu_prev_idle) : 0;
    if (wall_delta > 0) {
        double capacity = (double)wall_delta * (double)CONFIG_FREERTOS_NUMBER_OF_CORES;
        out->overall_percent = clamp_percent(100.0 -
            (((double)idle_delta * 100.0) / capacity));
        out->sample_window_ms = (uint32_t)(wall_delta / 1000U);
        out->valid = true;
    }
    s_cpu_prev_wall_us = wall_us;
    s_cpu_prev_idle = idle_runtime;
    s_cpu_usage_valid = true;
#endif
}

void si_device_observation_get_performance(si_device_performance_observation_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uptime_seconds = si_device_observation_uptime_seconds();
    out->task_count = uxTaskGetNumberOfTasks();
    collect_cpu(&out->cpu);
    collect_heap(&out->heap, MALLOC_CAP_8BIT);
    collect_heap(&out->internal_heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    collect_heap(&out->psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    si_device_observation_get_storage(&out->flash, &out->tf);
    si_device_observation_get_video(&out->video, NULL, NULL, 0, NULL);
}

void si_device_observation_log_append(const char *level, const char *message)
{
    si_device_log_observation_t entry = {0};
    uint32_t seconds = si_device_observation_uptime_seconds();
    snprintf(entry.time, sizeof(entry.time), "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
             seconds / 3600U, (seconds % 3600U) / 60U, seconds % 60U);
    strlcpy(entry.level, level ? level : "INFO", sizeof(entry.level));
    char *safe_message = si_utf8_sanitize(
        message ? message : "", sizeof(entry.message) - 1U);
    strlcpy(entry.message, safe_message ? safe_message : "",
            sizeof(entry.message));
    free(safe_message);

    taskENTER_CRITICAL(&s_log_mux);
    s_logs[s_log_head] = entry;
    s_log_head = (s_log_head + 1U) % SI_DEVICE_OBSERVATION_MAX_LOGS;
    if (s_log_count < SI_DEVICE_OBSERVATION_MAX_LOGS) {
        s_log_count++;
    }
    taskEXIT_CRITICAL(&s_log_mux);
}

size_t si_device_observation_log_snapshot(si_device_log_observation_t *out,
                                          size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    taskENTER_CRITICAL(&s_log_mux);
    size_t start = 0;
    size_t total = si_observation_recent_window(s_log_head, s_log_count,
                                                SI_DEVICE_OBSERVATION_MAX_LOGS,
                                                max_entries, &start);
    for (size_t i = 0; i < total; i++) {
        out[i] = s_logs[(start + i) % SI_DEVICE_OBSERVATION_MAX_LOGS];
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return total;
}
