#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES 128U
#define SI_DEVICE_OBSERVATION_MAX_POWER_GPIO 9U
#define SI_DEVICE_OBSERVATION_MAX_LOGS 80U

typedef struct {
    bool configured;
    bool connected;
    bool link_up;
    bool full_duplex;
    bool pending_confirmation;
    bool recovery_active;
    int retry_count;
    int speed_mbps;
    uint32_t config_generation;
    uint32_t confirm_remaining_seconds;
    char interface[16];
    char driver[32];
    char device_id[32];
    char hostname[64];
    char mode[12];
    char address_source[12];
    char config_state[16];
    char ip[16];
    char netmask[16];
    char gateway[16];
    char mac[18];
} si_device_network_observation_t;

typedef struct {
    bool selected;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    char pixel_format[16];
} si_device_video_mode_observation_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool streaming;
    bool frame_ready;
    bool capture_enabled;
    uint32_t width;
    uint32_t height;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_fps_x100;
    uint32_t capture_stride_ms;
    uint32_t modes_count;
    uint32_t data_lanes;
    uint32_t lane_bitrate_mbps;
    uint32_t jpeg_quality;
    uint32_t frames_captured;
    uint32_t frames_encoded;
    uint32_t frames_dropped;
    uint32_t last_jpeg_size;
    uint32_t frame_interval_ms;
    uint32_t last_frame_ms;
    uint32_t fps_x100;
    uint32_t uvc_callbacks;
    uint32_t uvc_queue_drops;
    uint32_t h264_pressure_coalesces;
    uint32_t uvc_buffer_underflows;
    uint32_t uvc_buffer_overflows;
    uint32_t uvc_return_pending;
    uint32_t uvc_return_retries;
    uint32_t uvc_return_failures;
    uint32_t ingest_validate_last_us;
    uint32_t ingest_validate_max_us;
    uint32_t ingest_publish_last_us;
    uint32_t ingest_publish_max_us;
    uint32_t ingest_total_last_us;
    uint32_t ingest_total_max_us;
    bool stream_metrics_valid;
    uint32_t stream_metrics_stream_id;
    uint32_t stream_fps_x100;
    uint32_t stream_bitrate_bps;
    uint32_t stream_sample_window_ms;
    char source[24];
    char pixel_format[16];
    char capture_owner[16];
    char last_error[96];
} si_device_video_observation_t;

typedef struct {
    bool preview_enabled;
    bool always_on;
    bool suspended;
    bool kvm_active;
    bool agent_active;
    bool agent_takeover;
    uint32_t kvm_remaining_ms;
    uint32_t agent_remaining_ms;
    uint32_t kvm_width;
    uint32_t kvm_height;
    uint32_t kvm_fps_x100;
    bool active_enabled;
    uint32_t active_width;
    uint32_t active_height;
    uint32_t active_fps_x100;
    uint32_t active_stride_ms;
    char active_owner[16];
    uint32_t kvm_grace_ms;
    uint32_t agent_grace_ms;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t preview_fps_x100;
    uint32_t preview_stride_ms;
} si_device_video_control_observation_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool mounted;
    bool ready;
    bool keyboard_writable;
    bool mouse_writable;
    bool absolute_pointer_writable;
    int dm_gpio;
    int dp_gpio;
    uint32_t tx_messages;
    uint32_t failed_messages;
    char mode[32];
    char port[32];
    char transport[24];
    char last_error[96];
} si_device_hid_observation_t;

typedef struct {
    char role[24];
    char label[32];
    char direction[12];
    int gpio;
    bool enabled;
    bool implemented;
    bool active_high;
    bool active;
    bool configurable;
    bool required;
    char state[24];
} si_device_power_gpio_observation_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool busy;
    bool active_high;
    bool power_detect_supported;
    bool standby_detect_supported;
    bool power_on;
    bool standby_on;
    bool locator_supported;
    bool locator_on;
    bool locator_bidirectional;
    bool locator_reversed;
    int power_button_gpio;
    int reset_button_gpio;
    int power_detect_gpio;
    int standby_detect_gpio;
    int locator_gpio;
    int locator_return_gpio;
    bool power_detect_active_high;
    bool standby_detect_active_high;
    bool locator_active_high;
    uint32_t default_press_ms;
    uint32_t force_off_ms;
    uint32_t action_count;
    char last_action[24];
    char last_error[96];
    size_t gpio_map_count;
    si_device_power_gpio_observation_t gpio_map[SI_DEVICE_OBSERVATION_MAX_POWER_GPIO];
} si_device_power_observation_t;

typedef struct {
    bool supported;
    bool initialized;
    bool power_on;
    bool rail_feedback_available;
    int switch_gpio;
    int core_enable_gpio;
    int switch_output_level;
    int core_enable_output_level;
    uint32_t operation_count;
    char state_kind[16];
    char last_result[32];
} si_device_ms2109_power_observation_t;

typedef struct {
    bool supported;
    bool mounted;
    bool degraded_mode;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    double usage_percent;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t bus_frequency_khz;
    uint8_t bus_width;
    char mount_point[16];
    char card_name[16];
    char card_type[16];
    char bus_mode[24];
    char fallback_reason[96];
    char last_error[96];
} si_device_tf_observation_t;

typedef struct {
    bool detected;
    uint32_t total_bytes;
    uint32_t partition_bytes;
    uint32_t reserved_bytes;
    uint32_t layout_gap_bytes;
    uint32_t app_partition_bytes;
    uint32_t data_partition_bytes;
    uint32_t free_bytes;
    int partition_count;
    bool nvs_stats_available;
    uint32_t nvs_partition_bytes;
    uint32_t nvs_total_entries;
    uint32_t nvs_used_entries;
    uint32_t nvs_free_entries;
    uint32_t nvs_namespace_count;
    char running_partition[17];
    char boot_partition[17];
    char next_update_partition[17];
    uint32_t running_partition_bytes;
    uint32_t next_update_partition_bytes;
} si_device_flash_observation_t;

typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t minimum_free_bytes;
    uint32_t largest_free_block;
} si_device_heap_observation_t;

typedef struct {
    bool valid;
    double overall_percent;
    uint32_t core_count;
    uint32_t sample_window_ms;
} si_device_cpu_observation_t;

typedef struct {
    bool active;
    bool kvm_view_active;
    bool kvm_active;
    bool agent_active;
    bool input_control_active;
    bool agent_takeover;
    bool can_request;
    uint32_t kvm_remaining_ms;
    uint32_t agent_remaining_ms;
    uint32_t expires_in_ms;
    uint32_t lease_ms;
    char owner[17];
    char mode[17];
    char agent_owner[17];
    char session_id[21];
    char reason[97];
} si_device_control_lease_observation_t;

typedef struct {
    uint32_t uptime_seconds;
    char uptime_formatted[32];
    uint32_t cpu_cores;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    uint32_t login_count;
    char device_label[49];
    char hostname[64];
    si_device_network_observation_t network;
    si_device_flash_observation_t flash;
    si_device_tf_observation_t tf;
} si_device_system_observation_t;

typedef struct {
    uint32_t uptime_seconds;
    uint32_t task_count;
    si_device_cpu_observation_t cpu;
    si_device_heap_observation_t heap;
    si_device_heap_observation_t internal_heap;
    si_device_heap_observation_t psram;
    si_device_flash_observation_t flash;
    si_device_tf_observation_t tf;
    si_device_video_observation_t video;
} si_device_performance_observation_t;

typedef struct {
    char time[16];
    char level[12];
    char message[128];
} si_device_log_observation_t;

uint32_t si_device_observation_uptime_seconds(void);
void si_device_observation_get_network(si_device_network_observation_t *out);
void si_device_observation_get_video(si_device_video_observation_t *out,
                                     si_device_video_control_observation_t *control_out,
                                     si_device_video_mode_observation_t *modes,
                                     size_t max_modes, size_t *mode_count_out);
void si_device_observation_get_hid(si_device_hid_observation_t *out);
void si_device_observation_get_power(si_device_power_observation_t *out);
void si_device_observation_get_ms2109_power(
    si_device_ms2109_power_observation_t *out);
void si_device_observation_get_storage(si_device_flash_observation_t *flash_out,
                                       si_device_tf_observation_t *tf_out);
void si_device_observation_get_control_lease(si_device_control_lease_observation_t *out);
void si_device_observation_get_system(si_device_system_observation_t *out);
void si_device_observation_get_performance(si_device_performance_observation_t *out);

void si_device_observation_log_append(const char *level, const char *message);
size_t si_device_observation_log_snapshot(si_device_log_observation_t *out,
                                          size_t max_entries);
