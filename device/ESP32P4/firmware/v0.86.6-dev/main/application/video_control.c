#include "video_control.h"

#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "settings_store.h"
#include "time_utils.h"
#include "video_input.h"

#define VIDEO_SETTINGS_NAMESPACE "si_video_cfg"
#define VIDEO_SETTINGS_KVM_WIDTH_KEY "kvm_width"
#define VIDEO_SETTINGS_KVM_HEIGHT_KEY "kvm_height"
#define VIDEO_SETTINGS_KVM_FPS_X100_KEY "kvm_fps_x100"
#define VIDEO_SETTINGS_QUALITY_KEY "quality"
#define VIDEO_SETTINGS_ALWAYS_ON_KEY "always_on"
#define VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY "preview_fps100"
#define VIDEO_CANCELED_STREAM_SLOTS 32U

typedef struct {
    bool enabled;
    bool exact_fps;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    uint32_t stride_ms;
    char owner[16];
} video_control_request_t;

static const char *TAG = "si-video-ctl";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_apply_lock;
static SemaphoreHandle_t s_transport_lock;
static TaskHandle_t s_task;
static TaskHandle_t s_hid_authority_task;
static bool s_preview_enabled;
static bool s_always_on;
static bool s_suspended;
static bool s_kvm_seen;
static uint32_t s_kvm_last_ms;
static uint32_t s_kvm_stream_id;
static uint32_t s_kvm_owner_epoch = 1U;
static uint32_t s_kvm_auth_generation;
static char s_kvm_session_id[SI_HID_OWNER_SESSION_ID_MAX_LEN + 1];
static uint32_t s_kvm_canceled_streams[VIDEO_CANCELED_STREAM_SLOTS];
static uint32_t s_kvm_canceled_cursor;
static uint32_t s_agent_stream_epoch;
static bool s_agent_seen;
static uint32_t s_agent_last_ms;
static bool s_agent_takeover;
static char s_agent_actor[8] = "agent";
static bool s_h264_transport_active;
static bool s_settings_loaded;
static uint32_t s_kvm_width = SI_CFG_VIDEO_WIDTH;
static uint32_t s_kvm_height = SI_CFG_VIDEO_HEIGHT;
static uint32_t s_kvm_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
static uint32_t s_preview_fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
static bool s_active_enabled;
static uint32_t s_active_width;
static uint32_t s_active_height;
static uint32_t s_active_fps_x100;
static uint32_t s_active_stride_ms;
static bool s_active_exact_fps;
static char s_active_owner[16] = "off";

static bool stream_was_canceled_locked(uint32_t stream_id)
{
    if (stream_id == 0U) {
        return false;
    }
    for (size_t i = 0; i < VIDEO_CANCELED_STREAM_SLOTS; i++) {
        if (s_kvm_canceled_streams[i] == stream_id) {
            return true;
        }
    }
    return false;
}

static void remember_canceled_stream_locked(uint32_t stream_id)
{
    if (stream_id == 0U || stream_was_canceled_locked(stream_id)) {
        return;
    }
    s_kvm_canceled_streams[
        s_kvm_canceled_cursor++ % VIDEO_CANCELED_STREAM_SLOTS] = stream_id;
}

static void forget_canceled_stream_locked(uint32_t stream_id)
{
    for (size_t i = 0; i < VIDEO_CANCELED_STREAM_SLOTS; i++) {
        if (s_kvm_canceled_streams[i] == stream_id) {
            s_kvm_canceled_streams[i] = 0U;
        }
    }
}

static uint32_t preview_stride_ms(uint32_t fps_x100)
{
    return fps_x100 > 0 ? (100000U + fps_x100 - 1U) / fps_x100 : 0;
}

static bool kvm_active_locked(uint32_t now_ms)
{
    return s_kvm_seen &&
           (uint32_t)(now_ms - s_kvm_last_ms) <= SI_VIDEO_CONTROL_KVM_GRACE_MS;
}

static uint32_t kvm_remaining_locked(uint32_t now_ms)
{
    if (!kvm_active_locked(now_ms)) {
        return 0;
    }
    return SI_VIDEO_CONTROL_KVM_GRACE_MS - (uint32_t)(now_ms - s_kvm_last_ms);
}

static bool agent_active_locked(uint32_t now_ms)
{
    return s_agent_seen &&
           (uint32_t)(now_ms - s_agent_last_ms) <= SI_VIDEO_CONTROL_AGENT_GRACE_MS;
}

static uint32_t agent_remaining_locked(uint32_t now_ms)
{
    if (!agent_active_locked(now_ms)) {
        return 0;
    }
    return SI_VIDEO_CONTROL_AGENT_GRACE_MS - (uint32_t)(now_ms - s_agent_last_ms);
}

static bool agent_takeover_active_locked(uint32_t now_ms)
{
    return s_agent_takeover && agent_active_locked(now_ms);
}

static const char *automation_actor_name(const char *actor)
{
    return actor && strcmp(actor, "mcp") == 0 ? "mcp" : "agent";
}

static void agent_epoch_bump_locked(void)
{
    if (++s_agent_stream_epoch == 0U) s_agent_stream_epoch = 1U;
}

static void desired_locked(video_control_request_t *request, uint32_t now_ms)
{
    memset(request, 0, sizeof(*request));
    strlcpy(request->owner, "off", sizeof(request->owner));

    if (s_suspended) {
        return;
    }
    /* A KVM viewer and the Agent may share the same capture stream.  Agent
     * takeover owns input, not observation.  Keeping the existing KVM mode
     * avoids renegotiating a healthy Stable-baseline UVC session down to the
     * low-rate Agent preview while a human is watching. */
    if (kvm_active_locked(now_ms)) {
        request->enabled = true;
        request->exact_fps = true;
        request->width = s_kvm_width;
        request->height = s_kvm_height;
        request->fps_x100 = s_kvm_fps_x100;
        /* Keep this request in product/UI terms.  video_input owns the
         * device-specific logical-mode to physical-transport mapping, so
         * MJPEG and H.264 use the same USB compatibility policy without
         * leaking a transport-only frame rate into saved settings. */
        strlcpy(request->owner, "kvm", sizeof(request->owner));
        return;
    }
    if (agent_takeover_active_locked(now_ms)) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "agent", sizeof(request->owner));
        return;
    }
    if (agent_active_locked(now_ms)) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "agent", sizeof(request->owner));
        return;
    }
    if (s_preview_enabled) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = s_preview_fps_x100;
        request->stride_ms = preview_stride_ms(s_preview_fps_x100);
        strlcpy(request->owner, "preview", sizeof(request->owner));
        return;
    }
    if (s_always_on) {
        request->enabled = true;
        request->width = SI_VIDEO_CONTROL_PREVIEW_WIDTH;
        request->height = SI_VIDEO_CONTROL_PREVIEW_HEIGHT;
        request->fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
        request->stride_ms = SI_VIDEO_CONTROL_PREVIEW_STRIDE_MS;
        strlcpy(request->owner, "always", sizeof(request->owner));
    }
}

static bool matches_active_locked(const video_control_request_t *request)
{
    if (request->enabled != s_active_enabled) {
        return false;
    }
    if (!request->enabled) {
        return strcmp(s_active_owner, "off") == 0;
    }
    return strcmp(request->owner, s_active_owner) == 0 &&
           request->width == s_active_width &&
           request->height == s_active_height &&
           request->fps_x100 == s_active_fps_x100 &&
           request->stride_ms == s_active_stride_ms &&
           request->exact_fps == s_active_exact_fps;
}

static void store_active_locked(const video_control_request_t *request)
{
    s_active_enabled = request->enabled;
    s_active_width = request->width;
    s_active_height = request->height;
    s_active_fps_x100 = request->fps_x100;
    s_active_stride_ms = request->stride_ms;
    s_active_exact_fps = request->exact_fps;
    strlcpy(s_active_owner, request->enabled ? request->owner : "off",
            sizeof(s_active_owner));
}

esp_err_t si_video_control_apply(void)
{
    if (!s_lock || !s_apply_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_apply_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    video_control_request_t request;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_apply_lock);
        return ESP_ERR_TIMEOUT;
    }
    desired_locked(&request, si_monotonic_ms());
    bool unchanged = matches_active_locked(&request);
    xSemaphoreGive(s_lock);
    if (unchanged) {
        xSemaphoreGive(s_apply_lock);
        return ESP_OK;
    }

    esp_err_t ret = si_video_set_capture(request.enabled, request.owner,
                                         request.width, request.height,
                                         request.fps_x100, request.exact_fps,
                                         request.stride_ms);
    if (ret == ESP_OK && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        store_active_locked(&request);
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_apply_lock);
    return ret;
}

static void kvm_epoch_bump_locked(void)
{
    if (++s_kvm_owner_epoch == 0U) s_kvm_owner_epoch = 1U;
}

static void preserve_hid_owner(const si_hid_owner_token_t *current,
                               si_hid_owner_transition_t *transition)
{
    if (current && current->generation != 0U) {
        transition->authorize = true;
        transition->claim = current->claim;
    }
}

static void set_kvm_hid_claim_locked(si_hid_owner_transition_t *transition)
{
    if (!s_kvm_seen || s_kvm_stream_id == 0U || !s_kvm_session_id[0] ||
        s_agent_takeover) return;
    transition->authorize = true;
    transition->claim.kind = SI_HID_OWNER_KVM_STREAM;
    transition->claim.auth_generation = s_kvm_auth_generation;
    transition->claim.resource_id = s_kvm_stream_id;
    transition->claim.authority_epoch = s_kvm_owner_epoch;
    strlcpy(transition->claim.session_id, s_kvm_session_id,
            sizeof(transition->claim.session_id));
}

typedef enum {
    KVM_OP_ACQUIRE,
    KVM_OP_RELEASE,
    KVM_OP_WS_CLAIM,
    KVM_OP_AGENT,
    KVM_OP_EXPIRE,
} kvm_operation_t;

typedef struct {
    kvm_operation_t operation;
    uint32_t stream_id;
    uint32_t expected_previous_stream_id;
    const char *session_id;
    uint32_t auth_generation;
    bool force;
    bool claim;
    bool takeover;
    const char *actor;
    bool replace;
    bool accepted;
    si_hid_live_guard_fn live_guard;
    void *guard_context;
} kvm_transition_context_t;

static bool kvm_transition_under_hid_gate(
    void *opaque, const si_hid_owner_token_t *current,
    si_hid_owner_transition_t *transition)
{
    kvm_transition_context_t *context = opaque;
    if (!context || !transition ||
        (context->live_guard &&
         !context->live_guard(context->guard_context)) || !s_lock ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    uint32_t now_ms = si_monotonic_ms();
    bool was_kvm_active = kvm_active_locked(now_ms);
    if (s_agent_takeover && !agent_active_locked(now_ms)) {
        s_agent_takeover = false;
    }

    switch (context->operation) {
    case KVM_OP_ACQUIRE: {
        bool heartbeat = s_kvm_stream_id == context->stream_id;
        bool canceled = stream_was_canceled_locked(context->stream_id);
        bool initial = context->claim && s_kvm_stream_id == 0U && !canceled;
        bool compare_exchange = context->claim &&
            context->expected_previous_stream_id != 0U &&
            s_kvm_stream_id == context->expected_previous_stream_id &&
            !canceled;
        bool expired = context->claim && !was_kvm_active && !canceled;
        bool live_heartbeat = heartbeat && was_kvm_active &&
            context->session_id &&
            strcmp(s_kvm_session_id, context->session_id) == 0 &&
            s_kvm_auth_generation == context->auth_generation;
        context->accepted = context->force || live_heartbeat || initial ||
                            compare_exchange || expired;
        if (!context->accepted ||
            (!context->session_id || !context->session_id[0])) break;
        if (context->force) {
            forget_canceled_stream_locked(context->stream_id);
            s_agent_seen = false;
            s_agent_takeover = false;
        }
        bool identity_changed = !live_heartbeat ||
            strcmp(s_kvm_session_id, context->session_id) != 0 ||
            s_kvm_auth_generation != context->auth_generation;
        if (!heartbeat) {
            remember_canceled_stream_locked(s_kvm_stream_id);
            s_kvm_stream_id = context->stream_id;
        }
        if (identity_changed) {
            kvm_epoch_bump_locked();
            if (++s_agent_stream_epoch == 0U) s_agent_stream_epoch = 1U;
        }
        s_kvm_seen = true;
        s_kvm_last_ms = now_ms;
        s_kvm_auth_generation = context->auth_generation;
        strlcpy(s_kvm_session_id, context->session_id,
                sizeof(s_kvm_session_id));
        break;
    }
    case KVM_OP_RELEASE:
        context->accepted = context->stream_id != 0U &&
            s_kvm_stream_id == context->stream_id && context->session_id &&
            strcmp(s_kvm_session_id, context->session_id) == 0 &&
            s_kvm_auth_generation == context->auth_generation;
        if (context->accepted) {
            remember_canceled_stream_locked(context->stream_id);
            s_kvm_seen = false;
            s_kvm_stream_id = 0U;
            s_kvm_session_id[0] = '\0';
            s_kvm_auth_generation = 0U;
            kvm_epoch_bump_locked();
        }
        break;
    case KVM_OP_WS_CLAIM:
        context->accepted = context->stream_id != 0U &&
            s_kvm_stream_id == context->stream_id && was_kvm_active &&
            !stream_was_canceled_locked(context->stream_id) &&
            !agent_takeover_active_locked(now_ms) && context->session_id &&
            context->session_id[0] && context->auth_generation != 0U &&
            strcmp(s_kvm_session_id, context->session_id) == 0 &&
            s_kvm_auth_generation == context->auth_generation;
        if (context->accepted) {
            kvm_epoch_bump_locked();
            s_kvm_auth_generation = context->auth_generation;
            strlcpy(s_kvm_session_id, context->session_id,
                    sizeof(s_kvm_session_id));
            transition->replace = true;
        }
        break;
    case KVM_OP_AGENT:
        if (!agent_active_locked(now_ms) ||
            strcmp(s_agent_actor,
                   automation_actor_name(context->actor)) != 0) {
            agent_epoch_bump_locked();
        }
        s_agent_seen = true;
        s_agent_last_ms = now_ms;
        s_agent_takeover = context->takeover;
        strlcpy(s_agent_actor, automation_actor_name(context->actor),
                sizeof(s_agent_actor));
        context->accepted = true;
        break;
    case KVM_OP_EXPIRE:
        context->accepted = s_kvm_seen && !was_kvm_active;
        if (context->accepted) {
            s_kvm_seen = false;
            s_kvm_session_id[0] = '\0';
            s_kvm_auth_generation = 0U;
            kvm_epoch_bump_locked();
        }
        break;
    }

    if (!context->accepted) {
        xSemaphoreGive(s_lock);
        return false;
    }
    if (context->operation == KVM_OP_AGENT && !context->takeover) {
        preserve_hid_owner(current, transition);
        xSemaphoreGive(s_lock);
        return true;
    }
    /* A KVM video heartbeat keeps observation alive, but it must not steal
     * HID back from an Agent/MCP control-lease token.  The control-lease
     * manager will clear that token on release/expiry; the browser then
     * reconnects and receives a fresh KVM token. */
    bool automated_input_owner = current &&
        current->claim.kind == SI_HID_OWNER_CONTROL_LEASE;
    bool wants_kvm = kvm_active_locked(now_ms) &&
                     !agent_takeover_active_locked(now_ms) &&
                     !automated_input_owner && s_kvm_session_id[0];
    if (wants_kvm) {
        set_kvm_hid_claim_locked(transition);
    } else if (current && current->claim.kind != SI_HID_OWNER_KVM_STREAM) {
        preserve_hid_owner(current, transition);
    }
    if (context->replace) transition->replace = true;
    xSemaphoreGive(s_lock);
    return true;
}

static bool run_kvm_transition(kvm_transition_context_t *context,
                               si_hid_owner_token_t *owner)
{
    return si_hid_authority_transition(kvm_transition_under_hid_gate,
                                       context, owner);
}

bool si_video_control_acquire_kvm_stream_for_session(
    uint32_t stream_id, bool force, bool claim,
    uint32_t expected_previous_stream_id, const char *session_id,
    uint32_t auth_generation, si_hid_live_guard_fn live_guard,
    void *guard_context)
{
    if (stream_id == 0U || !session_id || !session_id[0] ||
        auth_generation == 0U) return false;
    kvm_transition_context_t context = {
        .operation = KVM_OP_ACQUIRE, .stream_id = stream_id,
        .expected_previous_stream_id = expected_previous_stream_id,
        .session_id = session_id, .auth_generation = auth_generation,
        .force = force, .claim = claim, .live_guard = live_guard,
        .guard_context = guard_context,
    };
    const bool hid_handoff_ready = run_kvm_transition(&context, NULL);
    /* The transition callback is the linearization point for the revocable
     * video lease.  A mounted-but-not-writable HID function can fail the
     * subsequent neutral-report drain after that lease has been accepted.
     * Keep input fail-closed in that case, but do not misreport the already
     * live video lease as a stale stream: the HID WebSocket performs its own
     * exact owner claim before it can emit any report. */
    if (!hid_handoff_ready && context.accepted && claim) {
        ESP_LOGW(TAG, "KVM video lease accepted while HID handoff is pending");
    }
    return context.accepted;
}

bool si_video_control_claim_kvm_hid_owner(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context,
    si_hid_owner_token_t *owner)
{
    kvm_transition_context_t context = {
        .operation = KVM_OP_WS_CLAIM, .stream_id = stream_id,
        .session_id = session_id, .auth_generation = auth_generation,
        .replace = true, .live_guard = live_guard,
        .guard_context = guard_context,
    };
    return run_kvm_transition(&context, owner) && context.accepted;
}

bool si_video_control_kvm_hid_owner_is_current(
    uint32_t stream_id, uint32_t owner_epoch, const char *session_id,
    uint32_t auth_generation)
{
    if (!s_lock || stream_id == 0U || owner_epoch == 0U || !session_id ||
        !session_id[0] ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    uint32_t now_ms = si_monotonic_ms();
    bool current = s_kvm_seen && kvm_active_locked(now_ms) &&
        !agent_takeover_active_locked(now_ms) &&
        s_kvm_stream_id == stream_id && s_kvm_owner_epoch == owner_epoch &&
        s_kvm_auth_generation == auth_generation &&
        strcmp(s_kvm_session_id, session_id) == 0 &&
        !stream_was_canceled_locked(stream_id);
    xSemaphoreGive(s_lock);
    return current;
}

bool si_video_control_release_kvm_stream_for_session(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context)
{
    if (stream_id == 0U || !session_id || !session_id[0] ||
        auth_generation == 0U) return false;
    kvm_transition_context_t context = {
        .operation = KVM_OP_RELEASE, .stream_id = stream_id,
        .session_id = session_id, .auth_generation = auth_generation,
        .live_guard = live_guard, .guard_context = guard_context,
    };
    return run_kvm_transition(&context, NULL) && context.accepted;
}

bool si_video_control_kvm_stream_is_current(uint32_t stream_id)
{
    if (stream_id == 0U) {
        return true;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const bool current = s_kvm_stream_id == stream_id &&
                         !stream_was_canceled_locked(stream_id) &&
                         kvm_active_locked(si_monotonic_ms());
    xSemaphoreGive(s_lock);
    return current;
}

bool si_video_control_kvm_stream_can_control(uint32_t stream_id)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const uint32_t now_ms = si_monotonic_ms();
    const bool current =
        (stream_id == 0U ||
         (s_kvm_stream_id == stream_id &&
          !stream_was_canceled_locked(stream_id) &&
          kvm_active_locked(now_ms))) &&
        !agent_takeover_active_locked(now_ms);
    xSemaphoreGive(s_lock);
    return current;
}

uint32_t si_video_control_agent_stream_epoch(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0U;
    }
    uint32_t epoch = s_agent_stream_epoch;
    xSemaphoreGive(s_lock);
    return epoch;
}

void si_video_control_touch_automation(const char *actor, bool takeover)
{
    kvm_transition_context_t context = {
        .operation = KVM_OP_AGENT, .takeover = takeover, .actor = actor,
    };
    (void)run_kvm_transition(&context, NULL);
}

void si_video_control_keep_automation_alive(const char *actor)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now_ms = si_monotonic_ms();
        const char *normalized = automation_actor_name(actor);
        if (!agent_active_locked(now_ms) ||
            strcmp(s_agent_actor, normalized) != 0) {
            agent_epoch_bump_locked();
        }
        s_agent_seen = true;
        s_agent_last_ms = now_ms;
        strlcpy(s_agent_actor, normalized, sizeof(s_agent_actor));
        xSemaphoreGive(s_lock);
    }
}

void si_video_control_touch_agent(bool takeover)
{
    si_video_control_touch_automation("agent", takeover);
}

void si_video_control_keep_agent_alive(void)
{
    si_video_control_keep_automation_alive("agent");
}

void si_video_control_release_agent(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_agent_seen || s_agent_takeover) agent_epoch_bump_locked();
        s_agent_seen = false;
        s_agent_takeover = false;
        strlcpy(s_agent_actor, "agent", sizeof(s_agent_actor));
        xSemaphoreGive(s_lock);
    }
}

bool si_video_control_set_kvm_mode(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    if (!s_lock) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_kvm_width = width;
        s_kvm_height = height;
        s_kvm_fps_x100 = fps_x100 > 0 ? fps_x100 : s_kvm_fps_x100;
        if (s_kvm_fps_x100 == 0) {
            s_kvm_fps_x100 = SI_CFG_VIDEO_FPS * 100U;
        }
        active = kvm_active_locked(si_monotonic_ms());
        xSemaphoreGive(s_lock);
    }
    return active;
}

esp_err_t si_video_control_change_kvm_mode(uint32_t width, uint32_t height,
                                            uint32_t fps_x100,
                                            bool *applied_to_active_kvm)
{
    if (applied_to_active_kvm) {
        *applied_to_active_kvm = false;
    }
    if (!s_lock || !s_apply_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_apply_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t previous_width = 0U;
    uint32_t previous_height = 0U;
    uint32_t previous_fps_x100 = 0U;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_apply_lock);
        return ESP_ERR_TIMEOUT;
    }
    previous_width = s_kvm_width;
    previous_height = s_kvm_height;
    previous_fps_x100 = s_kvm_fps_x100;
    const uint32_t requested_fps_x100 =
        fps_x100 > 0U ? fps_x100 : previous_fps_x100;
    xSemaphoreGive(s_lock);

    if (!si_video_control_mode_valid(width, height,
                                     requested_fps_x100)) {
        xSemaphoreGive(s_apply_lock);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si_video_validate_mode(width, height,
                                           requested_fps_x100, true);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_apply_lock);
        return ret;
    }

    video_control_request_t request = {0};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_apply_lock);
        return ESP_ERR_TIMEOUT;
    }
    s_kvm_width = width;
    s_kvm_height = height;
    s_kvm_fps_x100 = requested_fps_x100;
    desired_locked(&request, si_monotonic_ms());
    const bool apply_to_kvm = request.enabled &&
                              strcmp(request.owner, "kvm") == 0;
    xSemaphoreGive(s_lock);

    if (apply_to_kvm) {
        ret = si_video_set_capture(request.enabled, request.owner,
                                   request.width, request.height,
                                   request.fps_x100, request.exact_fps,
                                   request.stride_ms);
    }
    if (ret == ESP_OK && apply_to_kvm) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            store_active_locked(&request);
            xSemaphoreGive(s_lock);
        }
    }
    if (ret != ESP_OK) {
        /* video_input keeps its active physical selection on rejection. Roll
         * back the control-plane request as part of the same serialized
         * transaction so the periodic apply task cannot retry a bad mode. */
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_kvm_width = previous_width;
            s_kvm_height = previous_height;
            s_kvm_fps_x100 = previous_fps_x100;
            xSemaphoreGive(s_lock);
        }
    } else if (applied_to_active_kvm) {
        *applied_to_active_kvm = apply_to_kvm;
    }
    xSemaphoreGive(s_apply_lock);
    return ret;
}

esp_err_t si_video_control_get_kvm_mode(uint32_t *width, uint32_t *height,
                                        uint32_t *fps_x100)
{
    if (!width || !height || !fps_x100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *width = s_kvm_width;
    *height = s_kvm_height;
    *fps_x100 = s_kvm_fps_x100;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool si_video_control_mode_valid(uint32_t width, uint32_t height, uint32_t fps_x100)
{
    return width >= SI_VIDEO_CONTROL_WIDTH_MIN && width <= SI_VIDEO_CONTROL_WIDTH_MAX &&
           height >= SI_VIDEO_CONTROL_HEIGHT_MIN && height <= SI_VIDEO_CONTROL_HEIGHT_MAX &&
           fps_x100 > 0 && fps_x100 <= SI_VIDEO_CONTROL_FPS_X100_MAX;
}

esp_err_t si_video_control_save_kvm_mode(uint32_t width, uint32_t height,
                                         uint32_t fps_x100)
{
    if (!si_video_control_mode_valid(width, height, fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u32(&store,
                                              VIDEO_SETTINGS_KVM_WIDTH_KEY,
                                              width);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_KVM_HEIGHT_KEY, height);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_KVM_FPS_X100_KEY,
                                        fps_x100);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_video_control_save_quality(uint32_t quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u32(&store,
                                              VIDEO_SETTINGS_QUALITY_KEY,
                                              quality);
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

bool si_video_control_preview_fps_valid(uint32_t fps_x100)
{
    return fps_x100 >= SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MIN &&
           fps_x100 <= SI_VIDEO_CONTROL_PREVIEW_FPS_X100_MAX &&
           fps_x100 % 100U == 0;
}

esp_err_t si_video_control_save_runtime_settings(bool always_on,
                                                  uint32_t preview_fps_x100)
{
    if (!si_video_control_preview_fps_valid(preview_fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(si_settings_store_open_write(&store,
                                                      VIDEO_SETTINGS_NAMESPACE),
                        TAG, "open video settings");
    esp_err_t ret = si_settings_store_set_u8(&store,
                                             VIDEO_SETTINGS_ALWAYS_ON_KEY,
                                             always_on ? 1 : 0);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u32(&store,
                                        VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY,
                                        preview_fps_x100);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    return ret;
}

esp_err_t si_video_control_set_always_on(bool enabled)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_always_on = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_control_set_preview_fps(uint32_t fps_x100)
{
    if (!si_video_control_preview_fps_valid(fps_x100)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_preview_fps_x100 = fps_x100;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void load_settings(void)
{
    if (s_settings_loaded) {
        return;
    }

    uint32_t width = SI_CFG_VIDEO_WIDTH;
    uint32_t height = SI_CFG_VIDEO_HEIGHT;
    uint32_t fps_x100 = SI_CFG_VIDEO_FPS * 100U;
    uint32_t preview_fps_x100 = SI_VIDEO_CONTROL_PREVIEW_FPS_X100;
    bool always_on = false;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store,
                                                 VIDEO_SETTINGS_NAMESPACE);
    if (err == ESP_OK) {
        uint32_t saved_width = 0;
        uint32_t saved_height = 0;
        uint32_t saved_fps_x100 = 0;
        bool have_width = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_WIDTH_KEY, &saved_width) == ESP_OK;
        bool have_height = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_HEIGHT_KEY, &saved_height) == ESP_OK;
        bool have_fps = si_settings_store_get_u32(
            &store, VIDEO_SETTINGS_KVM_FPS_X100_KEY,
            &saved_fps_x100) == ESP_OK;
        if (!have_fps) {
            saved_fps_x100 = fps_x100;
        }
        if (have_width && have_height &&
            si_video_control_mode_valid(saved_width, saved_height, saved_fps_x100)) {
            width = saved_width;
            height = saved_height;
            fps_x100 = saved_fps_x100;
        }

        uint32_t quality = 0;
        if (si_settings_store_get_u32(&store, VIDEO_SETTINGS_QUALITY_KEY,
                                      &quality) == ESP_OK &&
            quality >= 1 && quality <= 100) {
            (void)si_video_set_quality(quality);
        }
        uint8_t saved_always_on = 0;
        if (si_settings_store_get_u8(&store, VIDEO_SETTINGS_ALWAYS_ON_KEY,
                                     &saved_always_on) == ESP_OK) {
            always_on = saved_always_on != 0;
        }
        uint32_t saved_preview_fps_x100 = 0;
        if (si_settings_store_get_u32(&store,
                                      VIDEO_SETTINGS_PREVIEW_FPS_X100_KEY,
                                      &saved_preview_fps_x100) == ESP_OK &&
            si_video_control_preview_fps_valid(saved_preview_fps_x100)) {
            preview_fps_x100 = saved_preview_fps_x100;
        }
        si_settings_store_close(&store);
    }

    (void)si_video_control_set_kvm_mode(width, height, fps_x100);
    (void)si_video_control_set_always_on(always_on);
    (void)si_video_control_set_preview_fps(preview_fps_x100);
    s_settings_loaded = true;
}

esp_err_t si_video_control_set_preview(bool enabled)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_preview_enabled = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_video_control_set_suspended(bool suspended)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_suspended = suspended;
    xSemaphoreGive(s_lock);
    return si_video_control_apply();
}

esp_err_t si_video_control_set_h264_transport(bool active)
{
    /* The input driver uses this bit to arbitrate MJPEG/H.264 transport.
     * Physical UVC profile selection remains entirely inside video_input so
     * this control layer never rewrites the saved user-facing KVM mode. */
    if (!s_lock || !s_transport_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_transport_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_transport_lock);
        return ESP_ERR_TIMEOUT;
    }
    bool previous = s_h264_transport_active;
    bool changed = previous != active;
    s_h264_transport_active = active;
    xSemaphoreGive(s_lock);

    esp_err_t transport_ret = si_video_set_h264_transport_active(active);
    if (transport_ret != ESP_OK) {
        if (changed &&
            xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_h264_transport_active = previous;
            xSemaphoreGive(s_lock);
        }
        xSemaphoreGive(s_transport_lock);
        return transport_ret;
    }
    xSemaphoreGive(s_transport_lock);
    return ESP_OK;
}

void si_video_control_get_status(si_video_control_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    strlcpy(status->active_owner, "off", sizeof(status->active_owner));
    strlcpy(status->agent_actor, "agent", sizeof(status->agent_actor));
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    uint32_t now_ms = si_monotonic_ms();
    status->preview_enabled = s_preview_enabled;
    status->always_on = s_always_on;
    status->suspended = s_suspended;
    status->kvm_active = kvm_active_locked(now_ms);
    status->agent_active = agent_active_locked(now_ms);
    status->agent_takeover = agent_takeover_active_locked(now_ms);
    status->kvm_remaining_ms = kvm_remaining_locked(now_ms);
    status->agent_remaining_ms = agent_remaining_locked(now_ms);
    status->kvm_width = s_kvm_width;
    status->kvm_height = s_kvm_height;
    status->kvm_fps_x100 = s_kvm_fps_x100;
    status->preview_fps_x100 = s_preview_fps_x100;
    status->preview_stride_ms = preview_stride_ms(s_preview_fps_x100);
    status->active_enabled = s_active_enabled;
    status->active_width = s_active_width;
    status->active_height = s_active_height;
    status->active_fps_x100 = s_active_fps_x100;
    status->active_stride_ms = s_active_stride_ms;
    strlcpy(status->active_owner, s_active_owner, sizeof(status->active_owner));
    strlcpy(status->agent_actor, s_agent_actor, sizeof(status->agent_actor));
    xSemaphoreGive(s_lock);
}

static void control_task(void *arg)
{
    (void)arg;
    esp_err_t last_error = ESP_OK;
    while (true) {
        esp_err_t ret = si_video_control_apply();
        if (ret != ESP_OK && ret != last_error) {
            ESP_LOGW(TAG, "video control apply failed: %s", esp_err_to_name(ret));
        }
        last_error = ret;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void hid_authority_task(void *arg)
{
    (void)arg;
    while (true) {
        kvm_transition_context_t expiry = {.operation = KVM_OP_EXPIRE};
        (void)run_kvm_transition(&expiry, NULL);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t si_video_control_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create video control mutex");
    s_apply_lock = xSemaphoreCreateMutex();
    if (!s_apply_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_transport_lock = xSemaphoreCreateMutex();
    if (!s_transport_lock) {
        vSemaphoreDelete(s_apply_lock);
        s_apply_lock = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    video_control_request_t off = {0};
    strlcpy(off.owner, "off", sizeof(off.owner));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        store_active_locked(&off);
        xSemaphoreGive(s_lock);
    }

    BaseType_t ok;
#if SI_CFG_VIDEO_H264_ENABLED
    ok = xTaskCreatePinnedToCoreWithCaps(
        control_task, "si_video_ctl", 5120, NULL,
        tskIDLE_PRIORITY + 2, &s_task, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    ok = xTaskCreatePinnedToCore(control_task, "si_video_ctl", 5120, NULL,
                                 tskIDLE_PRIORITY + 2, &s_task,
                                 tskNO_AFFINITY);
#endif
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create video control task");
    ok = xTaskCreate(hid_authority_task, "si-kvm-own", 3072, NULL,
                     tskIDLE_PRIORITY + 3, &s_hid_authority_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create KVM HID authority task");
    ESP_RETURN_ON_ERROR(si_video_control_apply(), TAG, "apply initial video state");
    load_settings();
    return si_video_control_apply();
}
