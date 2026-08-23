#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_VIDEO_CONTROL_KVM_GRACE_MS 10000U
#define SI_VIDEO_CONTROL_AGENT_GRACE_MS 10000U
#define SI_VIDEO_CONTROL_PREVIEW_WIDTH 1280U
#define SI_VIDEO_CONTROL_PREVIEW_HEIGHT 720U
#define SI_VIDEO_CONTROL_PREVIEW_FPS_X100 100U
#define SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS 1000U
#define SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MIN 100U
#define SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MAX 1000U
#define SI_VIDEO_CONTROL_WIDTH_MIN 160U
#define SI_VIDEO_CONTROL_HEIGHT_MIN 120U
#define SI_VIDEO_CONTROL_WIDTH_MAX 7680U
#define SI_VIDEO_CONTROL_HEIGHT_MAX 4320U
#define SI_VIDEO_CONTROL_FPS_X100_MAX 24000U

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
    uint32_t preview_fps_x100;
    uint32_t preview_stride_ms;
    bool active_enabled;
    uint32_t active_width;
    uint32_t active_height;
    uint32_t active_fps_x100;
    uint32_t active_stride_ms;
    char active_owner[16];
} si_video_control_status_t;

esp_err_t si_video_control_start(void);
esp_err_t si_video_control_apply(void);
esp_err_t si_video_control_set_suspended(bool suspended);

bool si_video_control_touch_kvm(void);
void si_video_control_release_kvm(void);
void si_video_control_touch_agent(bool takeover);
void si_video_control_keep_agent_alive(void);
void si_video_control_release_agent(void);

bool si_video_control_set_kvm_mode(uint32_t width, uint32_t height, uint32_t fps_x100);
esp_err_t si_video_control_get_kvm_mode(uint32_t *width, uint32_t *height,
                                        uint32_t *fps_x100);
bool si_video_control_mode_valid(uint32_t width, uint32_t height, uint32_t fps_x100);
esp_err_t si_video_control_save_kvm_mode(uint32_t width, uint32_t height,
                                         uint32_t fps_x100);
esp_err_t si_video_control_save_quality(uint32_t quality);
bool si_video_control_preview_fps_valid(uint32_t fps_x100);
esp_err_t si_video_control_save_runtime_settings(bool always_on,
                                                  uint32_t preview_fps_x100);
esp_err_t si_video_control_set_always_on(bool enabled);
esp_err_t si_video_control_set_preview_fps(uint32_t fps_x100);
esp_err_t si_video_control_set_preview(bool enabled);

void si_video_control_get_status(si_video_control_status_t *status);
