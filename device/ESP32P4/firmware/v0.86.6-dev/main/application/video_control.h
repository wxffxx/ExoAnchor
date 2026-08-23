#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_owner.h"

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
#define SI_VIDEO_CONTROL_WIDTH_MAX 3840U
#define SI_VIDEO_CONTROL_HEIGHT_MAX 2160U
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
    char agent_actor[8];
} si_video_control_status_t;

esp_err_t si_video_control_start(void);
esp_err_t si_video_control_apply(void);
esp_err_t si_video_control_set_suspended(bool suspended);
esp_err_t si_video_control_set_h264_transport(bool active);

bool si_video_control_acquire_kvm_stream_for_session(
    uint32_t stream_id, bool force, bool claim,
    uint32_t expected_previous_stream_id, const char *session_id,
    uint32_t auth_generation, si_hid_live_guard_fn live_guard,
    void *guard_context);
bool si_video_control_claim_kvm_hid_owner(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context,
    si_hid_owner_token_t *owner);
bool si_video_control_kvm_hid_owner_is_current(
    uint32_t stream_id, uint32_t owner_epoch, const char *session_id,
    uint32_t auth_generation);
bool si_video_control_release_kvm_stream_for_session(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context);
bool si_video_control_kvm_stream_is_current(uint32_t stream_id);
bool si_video_control_kvm_stream_can_control(uint32_t stream_id);
uint32_t si_video_control_agent_stream_epoch(void);
void si_video_control_touch_automation(const char *actor, bool takeover);
void si_video_control_keep_automation_alive(const char *actor);
void si_video_control_touch_agent(bool takeover);
void si_video_control_keep_agent_alive(void);
void si_video_control_release_agent(void);

bool si_video_control_set_kvm_mode(uint32_t width, uint32_t height, uint32_t fps_x100);
esp_err_t si_video_control_change_kvm_mode(uint32_t width, uint32_t height,
                                            uint32_t fps_x100,
                                            bool *applied_to_active_kvm);
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
