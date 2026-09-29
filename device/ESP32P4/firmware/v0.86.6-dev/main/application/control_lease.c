#include "control_lease.h"

#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "time_utils.h"
#include "video_control.h"

static const char *TAG = "si-lease";

#define LEASE_MAINTENANCE_MS 50U
#define LEASE_PENDING_REVOKES 4U

typedef struct {
    bool used;
    char session_id[SI_CONTROL_LEASE_SESSION_ID_MAX_LEN + 1];
    uint32_t auth_generation;
} pending_revoke_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_maintenance_task;
static bool s_seen;
static uint32_t s_last_ms;
static uint32_t s_epoch = 1U;
static uint32_t s_resource_id;
static uint32_t s_next_embedded_resource_id;
static uint32_t s_auth_generation;
static char s_owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1] = "agent";
static char s_mode[SI_CONTROL_LEASE_MODE_MAX_LEN + 1] = "supervised";
static char s_session_id[SI_CONTROL_LEASE_SESSION_ID_MAX_LEN + 1] =
    "embedded-agent";
static char s_reason[SI_CONTROL_LEASE_REASON_MAX_LEN + 1];
/* Protected by the HID authority gate, not s_lock. */
static pending_revoke_t s_pending_revokes[LEASE_PENDING_REVOKES];
static bool s_pending_revoke_fail_closed;

static void epoch_bump_locked(void)
{
    if (++s_epoch == 0U) s_epoch = 1U;
}

static bool active_locked(uint32_t now_ms)
{
    return s_seen && (uint32_t)(now_ms - s_last_ms) <=
                         SI_CONTROL_LEASE_GRACE_MS;
}

static uint32_t remaining_locked(uint32_t now_ms)
{
    return active_locked(now_ms) ?
        SI_CONTROL_LEASE_GRACE_MS - (uint32_t)(now_ms - s_last_ms) : 0U;
}

static void video_snapshot(bool *kvm_view_active, bool *agent_takeover,
                           uint32_t *kvm_remaining_ms)
{
    si_video_control_status_t video;
    si_video_control_get_status(&video);
    bool kvm_view = video.kvm_active;
    if (kvm_view_active) *kvm_view_active = kvm_view;
    if (agent_takeover) *agent_takeover = video.agent_takeover;
    if (kvm_remaining_ms) {
        *kvm_remaining_ms = kvm_view ? video.kvm_remaining_ms : 0U;
    }
}

static void pending_revoke_add_under_hid_gate(const char *session_id,
                                              uint32_t auth_generation)
{
    for (size_t i = 0; i < LEASE_PENDING_REVOKES; i++) {
        if (s_pending_revokes[i].used &&
            strcmp(s_pending_revokes[i].session_id, session_id) == 0 &&
            s_pending_revokes[i].auth_generation == auth_generation) return;
    }
    for (size_t i = 0; i < LEASE_PENDING_REVOKES; i++) {
        if (!s_pending_revokes[i].used) {
            s_pending_revokes[i].used = true;
            strlcpy(s_pending_revokes[i].session_id, session_id,
                    sizeof(s_pending_revokes[i].session_id));
            s_pending_revokes[i].auth_generation = auth_generation;
            return;
        }
    }
    /* Never drop a cleanup obligation.  On journal pressure, the next
     * successful lease-lock acquisition clears the single active lease. */
    s_pending_revoke_fail_closed = true;
}

static bool pending_revokes_apply_locked(void)
{
    bool changed = false;
    if (s_pending_revoke_fail_closed) {
        if (s_seen) {
            s_seen = false;
            s_session_id[0] = '\0';
            s_reason[0] = '\0';
            s_auth_generation = 0U;
            s_resource_id = 0U;
            epoch_bump_locked();
            changed = true;
        }
        s_pending_revoke_fail_closed = false;
    }
    for (size_t i = 0; i < LEASE_PENDING_REVOKES; i++) {
        if (!s_pending_revokes[i].used) continue;
        bool matches = strcmp(s_session_id,
                              s_pending_revokes[i].session_id) == 0 &&
            (s_pending_revokes[i].auth_generation == 0U ||
             s_auth_generation == s_pending_revokes[i].auth_generation);
        if (matches && s_seen) {
            s_seen = false;
            s_session_id[0] = '\0';
            s_reason[0] = '\0';
            s_auth_generation = 0U;
            s_resource_id = 0U;
            epoch_bump_locked();
            changed = true;
        }
        memset(&s_pending_revokes[i], 0, sizeof(s_pending_revokes[i]));
    }
    return changed;
}

static bool materialize_expiry_locked(uint32_t now_ms)
{
    if (!s_seen || active_locked(now_ms)) return false;
    s_seen = false;
    s_reason[0] = '\0';
    s_resource_id = 0U;
    epoch_bump_locked();
    return true;
}

static void preserve_owner(const si_hid_owner_token_t *current,
                           si_hid_owner_transition_t *transition)
{
    if (current && current->generation != 0U) {
        transition->authorize = true;
        transition->claim = current->claim;
    }
}

static void desired_owner_locked(const si_hid_owner_token_t *current,
                                 uint32_t now_ms, bool kvm_view_active,
                                 si_hid_owner_transition_t *transition)
{
    /* A visible KVM page is an observer, not an input lock.  A live Agent/MCP
     * lease therefore replaces the passive KVM WebSocket owner while the
     * video stream stays online.  The browser can explicitly terminate that
     * lease and reconnect its HID socket to resume manual input. */
    if (active_locked(now_ms) && strcasecmp(s_mode, "observe") != 0) {
        transition->authorize = true;
        transition->claim.kind = SI_HID_OWNER_CONTROL_LEASE;
        transition->claim.auth_generation = s_auth_generation;
        transition->claim.resource_id = s_resource_id;
        transition->claim.authority_epoch = s_epoch;
        strlcpy(transition->claim.session_id, s_session_id,
                sizeof(transition->claim.session_id));
        return;
    }
    if (kvm_view_active && current &&
        current->claim.kind == SI_HID_OWNER_KVM_STREAM) {
        preserve_owner(current, transition);
        return;
    }
    /* Lease cleanup must never clear an unrelated KVM report owner. */
    if (current && current->claim.kind != SI_HID_OWNER_CONTROL_LEASE) {
        preserve_owner(current, transition);
    }
}

typedef enum {
    LEASE_OP_TOUCH_AGENT,
    LEASE_OP_TOUCH_BOOT,
    LEASE_OP_RELEASE_EMBEDDED,
    LEASE_OP_UPDATE,
    LEASE_OP_REVOKE_SESSION,
    LEASE_OP_MAINTAIN,
} lease_operation_t;

typedef struct {
    lease_operation_t operation;
    const char *owner;
    const char *session_id;
    const char *mode;
    const char *reason;
    uint32_t auth_generation;
    const si_hid_owner_token_t *expected_owner;
    bool active;
    bool force;
    si_hid_live_guard_fn live_guard;
    void *guard_context;
    esp_err_t error;
    si_control_lease_update_result_t update_result;
    bool state_changed;
} lease_transition_context_t;

static bool lease_transition_under_hid_gate(
    void *opaque, const si_hid_owner_token_t *current,
    si_hid_owner_transition_t *transition)
{
    lease_transition_context_t *context = opaque;
    if (!context || !transition || !s_lock) return false;

    /* The HTTP authentication snapshot is only a hint.  Revalidate the exact
     * session/generation/capability at the owner-transition linearization
     * boundary so logout cannot race a stale request into a new lease. */
    if (context->operation == LEASE_OP_UPDATE &&
        (!context->live_guard ||
         !context->live_guard(context->guard_context))) {
        context->error = ESP_ERR_INVALID_STATE;
        context->update_result = SI_CONTROL_LEASE_UPDATE_INVALID;
        return false;
    }

    bool kvm_view_active = false;
    video_snapshot(&kvm_view_active, NULL, NULL);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (context->operation == LEASE_OP_REVOKE_SESSION &&
            context->session_id && context->session_id[0]) {
            pending_revoke_add_under_hid_gate(
                context->session_id, context->auth_generation);
            context->state_changed = true;
        }
        context->error = ESP_ERR_TIMEOUT;
        context->update_result = SI_CONTROL_LEASE_UPDATE_UNAVAILABLE;
        return false;
    }

    uint32_t now_ms = si_monotonic_ms();
    context->state_changed = pending_revokes_apply_locked();
    context->state_changed = materialize_expiry_locked(now_ms) ||
                             context->state_changed;
    bool was_active = active_locked(now_ms);

    switch (context->operation) {
    case LEASE_OP_TOUCH_AGENT:
    case LEASE_OP_TOUCH_BOOT: {
        const char *mode = context->operation == LEASE_OP_TOUCH_BOOT ?
                           "supervised" : context->mode;
        const char *reason = context->operation == LEASE_OP_TOUCH_BOOT ?
                             "boot-key sequence" : context->reason;
        si_hid_embedded_touch_decision_t touch =
            si_hid_embedded_touch_decide(
                was_active, current, context->expected_owner,
                s_resource_id, s_epoch);
        bool renewal = touch == SI_HID_EMBEDDED_TOUCH_RENEW;
        if (touch == SI_HID_EMBEDDED_TOUCH_DENY) {
            context->error = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(s_lock);
            return false;
        }
        bool changed = !renewal || strcasecmp(s_mode, mode) != 0;
        s_seen = true;
        s_last_ms = now_ms;
        s_auth_generation = 0U;
        strlcpy(s_owner, "agent", sizeof(s_owner));
        strlcpy(s_mode, mode, sizeof(s_mode));
        strlcpy(s_session_id, "embedded-agent", sizeof(s_session_id));
        strlcpy(s_reason, reason ? reason : "", sizeof(s_reason));
        if (!renewal) {
            if (++s_next_embedded_resource_id == 0U) {
                s_next_embedded_resource_id = 1U;
            }
            s_resource_id = s_next_embedded_resource_id;
            epoch_bump_locked();
        } else if (changed) {
            epoch_bump_locked();
        }
        context->state_changed = changed || context->state_changed;
        break;
    }
    case LEASE_OP_RELEASE_EMBEDDED:
        if (si_hid_embedded_release_matches(
                was_active, context->expected_owner, s_resource_id,
                s_epoch)) {
            /* The report owner may already be a manual KVM token.  Clear this
             * exact producer's dormant lease state without touching that KVM;
             * desired_owner_locked preserves unrelated current owners. */
            s_seen = false;
            s_reason[0] = '\0';
            s_resource_id = 0U;
            epoch_bump_locked();
            context->state_changed = true;
        } else {
            context->error = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(s_lock);
            return false;
        }
        break;
    case LEASE_OP_UPDATE: {
        bool other = context->active && was_active &&
            (strcasecmp(s_owner, context->owner) != 0 ||
             strcmp(s_session_id, context->session_id) != 0 ||
             s_auth_generation != context->auth_generation);
        if (other && !context->force) {
            context->update_result = SI_CONTROL_LEASE_UPDATE_HELD_BY_OTHER;
            xSemaphoreGive(s_lock);
            return false;
        }
        if (context->active) {
            bool changed = !was_active || other ||
                strcasecmp(s_mode, context->mode) != 0;
            s_seen = true;
            s_last_ms = now_ms;
            s_auth_generation = context->auth_generation;
            s_resource_id = 0U;
            strlcpy(s_owner, context->owner, sizeof(s_owner));
            strlcpy(s_mode, context->mode, sizeof(s_mode));
            strlcpy(s_session_id, context->session_id, sizeof(s_session_id));
            strlcpy(s_reason, context->reason ? context->reason : "",
                    sizeof(s_reason));
            if (changed) epoch_bump_locked();
            context->state_changed = changed || context->state_changed;
        } else if (!was_active || context->force ||
                   (strcasecmp(s_owner, context->owner) == 0 &&
                    strcmp(s_session_id, context->session_id) == 0 &&
                    (context->auth_generation == 0U ||
                     s_auth_generation == context->auth_generation))) {
            if (was_active) epoch_bump_locked();
            s_seen = false;
            s_reason[0] = '\0';
            s_resource_id = 0U;
            context->state_changed = was_active || context->state_changed;
        }
        context->update_result = SI_CONTROL_LEASE_UPDATE_OK;
        break;
    }
    case LEASE_OP_REVOKE_SESSION:
        if (context->session_id &&
            strcmp(s_session_id, context->session_id) == 0 &&
            (context->auth_generation == 0U ||
             s_auth_generation == context->auth_generation)) {
            if (s_seen) epoch_bump_locked();
            s_seen = false;
            s_session_id[0] = '\0';
            s_reason[0] = '\0';
            s_auth_generation = 0U;
            s_resource_id = 0U;
            context->state_changed = true;
        }
        break;
    case LEASE_OP_MAINTAIN:
        break;
    }

    desired_owner_locked(current, now_ms, kvm_view_active, transition);
    xSemaphoreGive(s_lock);
    context->error = ESP_OK;
    return true;
}

static bool run_transition(lease_transition_context_t *context,
                           si_hid_owner_token_t *owner)
{
    return si_hid_authority_transition(lease_transition_under_hid_gate,
                                       context, owner);
}

static void lease_maintenance(void *arg)
{
    (void)arg;
    while (true) {
        lease_transition_context_t context = {
            .operation = LEASE_OP_MAINTAIN,
        };
        (void)run_transition(&context, NULL);
        vTaskDelay(pdMS_TO_TICKS(LEASE_MAINTENANCE_MS));
    }
}

static bool embedded_lease_live_under_hid_gate(void *opaque)
{
    (void)opaque;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool live = active_locked(si_monotonic_ms()) &&
                strcasecmp(s_owner, "agent") == 0 &&
                strcmp(s_session_id, "embedded-agent") == 0 &&
                s_auth_generation == 0U &&
                strcasecmp(s_mode, "observe") != 0;
    xSemaphoreGive(s_lock);
    return live;
}

esp_err_t si_control_lease_start(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create control lease mutex");
    BaseType_t created = xTaskCreate(lease_maintenance, "si-lease-own",
                                    3072, NULL, 5, &s_maintenance_task);
    if (created != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t guard_ret = si_hid_set_embedded_authority_guard(
        embedded_lease_live_under_hid_gate, NULL);
    if (guard_ret != ESP_OK) {
        vTaskDelete(s_maintenance_task);
        s_maintenance_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return guard_ret;
    }
    return ESP_OK;
}

bool si_control_lease_mode_valid(const char *mode)
{
    return mode && (strcasecmp(mode, "observe") == 0 ||
                    strcasecmp(mode, "supervised") == 0 ||
                    strcasecmp(mode, "autonomous") == 0);
}

bool si_control_lease_owner_valid(const char *owner)
{
    return owner && (strcasecmp(owner, "agent") == 0 ||
                     strcasecmp(owner, "mcp") == 0);
}

static esp_err_t touch_embedded_owned(lease_operation_t operation,
                                     const char *mode, const char *reason,
                                     si_hid_owner_token_t *owner)
{
    if (!owner || !si_control_lease_mode_valid(mode)) return ESP_ERR_INVALID_ARG;
    si_hid_owner_token_t expected = *owner;
    lease_transition_context_t context = {
        .operation = operation, .mode = mode, .reason = reason,
        .active = true, .error = ESP_FAIL,
        .expected_owner = expected.generation != 0U ? &expected : NULL,
    };
    si_hid_owner_token_t granted = {0};
    if (!run_transition(&context, &granted)) return context.error;
    esp_err_t bind_ret = si_hid_bind_embedded_producer(&granted);
    if (bind_ret != ESP_OK) {
        lease_transition_context_t release = {
            .operation = LEASE_OP_RELEASE_EMBEDDED,
            .expected_owner = &granted,
        };
        (void)run_transition(&release, NULL);
        return bind_ret;
    }
    *owner = granted;
    return ESP_OK;
}

esp_err_t si_control_lease_touch_agent_owned(
    const char *mode, const char *reason, si_hid_owner_token_t *owner)
{
    return touch_embedded_owned(LEASE_OP_TOUCH_AGENT, mode, reason, owner);
}

esp_err_t si_control_lease_touch_boot_sequence_owned(
    si_hid_owner_token_t *owner)
{
    return touch_embedded_owned(LEASE_OP_TOUCH_BOOT, "supervised",
                                "boot-key sequence", owner);
}

bool si_control_lease_release_embedded_owner(
    const si_hid_owner_token_t *owner)
{
    if (!owner || owner->generation == 0U) return false;
    lease_transition_context_t context = {
        .operation = LEASE_OP_RELEASE_EMBEDDED,
        .expected_owner = owner,
    };
    return run_transition(&context, NULL) && context.state_changed;
}

void si_control_lease_release_agent(void)
{
    si_hid_owner_token_t owner = {0};
    if (si_hid_get_bound_embedded_owner(&owner)) {
        (void)si_control_lease_release_embedded_owner(&owner);
    }
}

bool si_control_lease_claim_kvm_hid_owner(
    uint32_t stream_id, const char *session_id, uint32_t auth_generation,
    si_hid_live_guard_fn live_guard, void *guard_context,
    si_hid_owner_token_t *owner)
{
    return si_video_control_claim_kvm_hid_owner(
        stream_id, session_id, auth_generation, live_guard, guard_context,
        owner);
}

bool si_control_lease_kvm_hid_owner_is_current(
    uint32_t stream_id, uint32_t owner_epoch, const char *session_id,
    uint32_t auth_generation)
{
    return si_video_control_kvm_hid_owner_is_current(
        stream_id, owner_epoch, session_id, auth_generation);
}

bool si_control_lease_agent_active(void)
{
    if (!s_lock) return false;
    bool active = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = active_locked(si_monotonic_ms());
        xSemaphoreGive(s_lock);
    }
    return active;
}

bool si_control_lease_agent_allows_actions(void)
{
    if (!s_lock) return false;
    bool allowed = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        allowed = active_locked(si_monotonic_ms()) &&
                  strcasecmp(s_mode, "observe") != 0;
        xSemaphoreGive(s_lock);
    }
    return allowed;
}

bool si_control_lease_owner_matches(const char *owner)
{
    if (!owner || !s_lock) return false;
    bool matches = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        matches = active_locked(si_monotonic_ms()) &&
                  strcasecmp(s_owner, owner) == 0;
        xSemaphoreGive(s_lock);
    }
    return matches;
}

bool si_control_lease_owner_session_matches(const char *owner,
                                            const char *session_id)
{
    if (!owner || !session_id || !session_id[0] || !s_lock) return false;
    bool matches = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        matches = active_locked(si_monotonic_ms()) &&
                  strcasecmp(s_owner, owner) == 0 &&
                  strcmp(s_session_id, session_id) == 0;
        xSemaphoreGive(s_lock);
    }
    return matches;
}

bool si_control_lease_get_hid_owner(const char *owner,
                                    const char *session_id,
                                    uint32_t auth_generation,
                                    uint32_t expected_epoch,
                                    si_hid_owner_token_t *token)
{
    if (!owner || !session_id || !token || !s_lock ||
        !si_hid_owner_get_current(token) ||
        token->claim.kind != SI_HID_OWNER_CONTROL_LEASE ||
        token->claim.auth_generation != auth_generation ||
        token->claim.authority_epoch != expected_epoch ||
        strcmp(token->claim.session_id, session_id) != 0) return false;
    bool matches = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        matches = active_locked(si_monotonic_ms()) &&
                  strcasecmp(s_mode, "observe") != 0 &&
                  strcasecmp(s_owner, owner) == 0 &&
                  strcmp(s_session_id, session_id) == 0 &&
                  s_auth_generation == auth_generation &&
                  s_epoch == expected_epoch;
        xSemaphoreGive(s_lock);
    }
    return matches;
}

bool si_control_lease_revoke_auth_session(const char *session_id,
                                          uint32_t auth_generation)
{
    if (!session_id || !session_id[0]) return false;
    bool hid_revoked = si_hid_owner_revoke_session(session_id,
                                                   auth_generation);
    lease_transition_context_t context = {
        .operation = LEASE_OP_REVOKE_SESSION, .session_id = session_id,
        .auth_generation = auth_generation,
    };
    bool cleaned = run_transition(&context, NULL);
    return hid_revoked || cleaned || context.state_changed;
}

si_control_lease_update_result_t si_control_lease_update_for_auth_session(
    const char *owner, const char *session_id, uint32_t auth_generation,
    const char *mode, const char *reason, bool active, bool force,
    si_hid_live_guard_fn live_guard, void *guard_context)
{
    if (!si_control_lease_owner_valid(owner) || !session_id ||
        !session_id[0] ||
        strlen(session_id) > SI_CONTROL_LEASE_SESSION_ID_MAX_LEN ||
        (active && !si_control_lease_mode_valid(mode))) {
        return SI_CONTROL_LEASE_UPDATE_INVALID;
    }
    lease_transition_context_t context = {
        .operation = LEASE_OP_UPDATE, .owner = owner,
        .session_id = session_id, .mode = mode, .reason = reason,
        .auth_generation = auth_generation, .active = active,
        .force = force, .live_guard = live_guard,
        .guard_context = guard_context,
        .update_result = SI_CONTROL_LEASE_UPDATE_UNAVAILABLE,
    };
    (void)run_transition(&context, NULL);
    return context.update_result;
}

void si_control_lease_get_status(si_control_lease_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    strlcpy(status->owner, "none", sizeof(status->owner));
    strlcpy(status->mode, "idle", sizeof(status->mode));
    strlcpy(status->agent_owner, "agent", sizeof(status->agent_owner));
    video_snapshot(&status->kvm_view_active, &status->agent_takeover,
                   &status->kvm_remaining_ms);
    si_hid_owner_token_t hid_owner = {0};
    bool hid_owner_present = si_hid_owner_get_current(&hid_owner);
    status->kvm_active = status->kvm_view_active && hid_owner_present &&
        hid_owner.claim.kind == SI_HID_OWNER_KVM_STREAM;
    bool raw_agent_active = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        status->epoch = s_epoch;
        uint32_t now_ms = si_monotonic_ms();
        raw_agent_active = active_locked(now_ms);
        status->agent_active = raw_agent_active;
        status->agent_remaining_ms = remaining_locked(now_ms);
        strlcpy(status->agent_owner, s_owner, sizeof(status->agent_owner));
        strlcpy(status->session_id, s_session_id, sizeof(status->session_id));
        strlcpy(status->reason, s_reason, sizeof(status->reason));
        if (raw_agent_active) {
            strlcpy(status->mode, s_mode, sizeof(status->mode));
            status->input_control_active =
                strcasecmp(s_mode, "observe") != 0 && hid_owner_present &&
                hid_owner.claim.kind == SI_HID_OWNER_CONTROL_LEASE &&
                hid_owner.claim.auth_generation == s_auth_generation &&
                hid_owner.claim.authority_epoch == s_epoch &&
                strcmp(hid_owner.claim.session_id, s_session_id) == 0;
        }
        xSemaphoreGive(s_lock);
    }
    if (raw_agent_active) {
        strlcpy(status->owner, status->agent_owner, sizeof(status->owner));
        status->expires_in_ms = status->agent_remaining_ms;
    } else if (status->kvm_active) {
        strlcpy(status->owner, "kvm", sizeof(status->owner));
        strlcpy(status->mode, "manual", sizeof(status->mode));
        status->expires_in_ms = status->kvm_remaining_ms;
    }
    status->active = status->kvm_active || raw_agent_active;
    status->can_request = !raw_agent_active;
}
