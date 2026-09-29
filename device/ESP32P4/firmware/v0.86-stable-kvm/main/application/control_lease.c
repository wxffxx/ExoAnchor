#include "control_lease.h"

#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_utils.h"
#include "video_control.h"

static const char *TAG = "si-lease";

static SemaphoreHandle_t s_lock;
static bool s_seen;
static uint32_t s_last_ms;
static char s_owner[SI_CONTROL_LEASE_OWNER_MAX_LEN + 1] = "agent";
static char s_mode[SI_CONTROL_LEASE_MODE_MAX_LEN + 1] = "supervised";
static char s_reason[SI_CONTROL_LEASE_REASON_MAX_LEN + 1];

static bool active_locked(uint32_t now_ms)
{
    return s_seen && (uint32_t)(now_ms - s_last_ms) <= SI_CONTROL_LEASE_GRACE_MS;
}

static uint32_t remaining_locked(uint32_t now_ms)
{
    if (!active_locked(now_ms)) {
        return 0;
    }
    return SI_CONTROL_LEASE_GRACE_MS - (uint32_t)(now_ms - s_last_ms);
}

static void video_snapshot(bool *kvm_active, bool *agent_takeover,
                           uint32_t *kvm_remaining_ms)
{
    si_video_control_status_t video;
    si_video_control_get_status(&video);
    bool kvm = video.kvm_active && !video.agent_takeover;
    if (kvm_active) {
        *kvm_active = kvm;
    }
    if (agent_takeover) {
        *agent_takeover = video.agent_takeover;
    }
    if (kvm_remaining_ms) {
        *kvm_remaining_ms = kvm ? video.kvm_remaining_ms : 0;
    }
}

esp_err_t si_control_lease_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create control lease mutex");
    return ESP_OK;
}

bool si_control_lease_mode_valid(const char *mode)
{
    return mode &&
           (strcasecmp(mode, "observe") == 0 ||
            strcasecmp(mode, "supervised") == 0 ||
            strcasecmp(mode, "autonomous") == 0);
}

bool si_control_lease_owner_valid(const char *owner)
{
    return owner &&
           (strcasecmp(owner, "agent") == 0 ||
            strcasecmp(owner, "mcp") == 0);
}

esp_err_t si_control_lease_touch_agent(const char *mode, const char *reason)
{
    if (!si_control_lease_mode_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool kvm_active = false;
    video_snapshot(&kvm_active, NULL, NULL);
    if (kvm_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t now_ms = si_monotonic_ms();
    if (active_locked(now_ms) && strcasecmp(s_owner, "agent") != 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_seen = true;
    s_last_ms = now_ms;
    strlcpy(s_owner, "agent", sizeof(s_owner));
    strlcpy(s_mode, mode, sizeof(s_mode));
    strlcpy(s_reason, reason ? reason : "", sizeof(s_reason));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool si_control_lease_agent_active(void)
{
    bool kvm_active = false;
    video_snapshot(&kvm_active, NULL, NULL);
    if (kvm_active || !s_lock) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = active_locked(si_monotonic_ms());
        xSemaphoreGive(s_lock);
    }
    return active;
}

bool si_control_lease_agent_allows_actions(void)
{
    bool kvm_active = false;
    video_snapshot(&kvm_active, NULL, NULL);
    if (kvm_active || !s_lock) {
        return false;
    }
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
    if (!owner || !s_lock) {
        return false;
    }
    bool matches = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        matches = active_locked(si_monotonic_ms()) &&
                  strcasecmp(s_owner, owner) == 0;
        xSemaphoreGive(s_lock);
    }
    return matches;
}

si_control_lease_update_result_t si_control_lease_update(const char *owner,
                                                         const char *mode,
                                                         const char *reason,
                                                         bool active,
                                                         bool force)
{
    if (!si_control_lease_owner_valid(owner) ||
        (active && !si_control_lease_mode_valid(mode))) {
        return SI_CONTROL_LEASE_UPDATE_INVALID;
    }
    bool kvm_active = false;
    video_snapshot(&kvm_active, NULL, NULL);
    if (active && kvm_active) {
        return SI_CONTROL_LEASE_UPDATE_KVM_ACTIVE;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return SI_CONTROL_LEASE_UPDATE_UNAVAILABLE;
    }

    uint32_t now_ms = si_monotonic_ms();
    bool other_owner_active = active && active_locked(now_ms) &&
                              strcasecmp(s_owner, owner) != 0;
    if (other_owner_active && !force) {
        xSemaphoreGive(s_lock);
        return SI_CONTROL_LEASE_UPDATE_HELD_BY_OTHER;
    }

    if (active) {
        s_seen = true;
        s_last_ms = now_ms;
        strlcpy(s_owner, owner, sizeof(s_owner));
        strlcpy(s_mode, mode, sizeof(s_mode));
        strlcpy(s_reason, reason ? reason : "", sizeof(s_reason));
    } else if (!active_locked(now_ms) || strcasecmp(s_owner, owner) == 0 || force) {
        s_seen = false;
        strlcpy(s_owner, owner, sizeof(s_owner));
        s_reason[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    return SI_CONTROL_LEASE_UPDATE_OK;
}

void si_control_lease_get_status(si_control_lease_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    strlcpy(status->owner, "none", sizeof(status->owner));
    strlcpy(status->mode, "idle", sizeof(status->mode));
    strlcpy(status->agent_owner, "agent", sizeof(status->agent_owner));

    video_snapshot(&status->kvm_active, &status->agent_takeover,
                   &status->kvm_remaining_ms);
    bool raw_agent_active = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now_ms = si_monotonic_ms();
        raw_agent_active = active_locked(now_ms);
        status->agent_active = raw_agent_active && !status->kvm_active;
        status->agent_remaining_ms = remaining_locked(now_ms);
        strlcpy(status->agent_owner, s_owner, sizeof(status->agent_owner));
        strlcpy(status->reason, s_reason, sizeof(status->reason));
        if (raw_agent_active) {
            strlcpy(status->mode, s_mode, sizeof(status->mode));
        }
        xSemaphoreGive(s_lock);
    }
    if (status->kvm_active) {
        strlcpy(status->owner, "kvm", sizeof(status->owner));
        strlcpy(status->mode, "manual", sizeof(status->mode));
        status->expires_in_ms = status->kvm_remaining_ms;
    } else if (raw_agent_active) {
        strlcpy(status->owner, status->agent_owner, sizeof(status->owner));
        status->expires_in_ms = status->agent_remaining_ms;
    }
    status->active = status->kvm_active || status->agent_active;
    status->can_request = !status->kvm_active;
}
