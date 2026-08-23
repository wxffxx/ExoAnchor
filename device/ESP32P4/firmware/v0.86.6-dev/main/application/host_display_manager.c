#include "host_display_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_utils.h"

static const char *TAG = "si-host-display";

static SemaphoreHandle_t s_lock;
static si_host_display_status_t s_status;
static uint32_t s_plan_seq;

static bool plan_matches_locked(const char *plan_id)
{
    return plan_id && plan_id[0] && s_status.plan_id[0] &&
           strcmp(plan_id, s_status.plan_id) == 0;
}

static void copy_out_locked(si_host_display_status_t *out)
{
    if (out) {
        *out = s_status;
    }
}

static bool state_accepts_new_plan(si_host_display_state_t state)
{
    return state == SI_HOST_DISPLAY_STATE_EMPTY ||
           state == SI_HOST_DISPLAY_STATE_CANCELLED ||
           state == SI_HOST_DISPLAY_STATE_VERIFIED ||
           state == SI_HOST_DISPLAY_STATE_ROLLED_BACK ||
           state == SI_HOST_DISPLAY_STATE_FAILED;
}

static esp_err_t take_lock(void)
{
    return s_lock &&
                   xSemaphoreTake(s_lock, pdMS_TO_TICKS(250)) == pdTRUE ?
               ESP_OK :
               ESP_ERR_TIMEOUT;
}

static esp_err_t transition_begin(
    const char *plan_id, si_host_display_state_t from_a,
    si_host_display_state_t from_b, si_host_display_state_t next,
    si_host_display_status_t *out)
{
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != from_a && s_status.state != from_b) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = next;
    s_status.updated_ms = si_monotonic_ms();
    s_status.error[0] = '\0';
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t transition_finish(
    const char *plan_id, si_host_display_state_t expected,
    si_host_display_state_t success_state, bool success, const char *error,
    si_host_display_status_t *out)
{
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != expected) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = success ? success_state : SI_HOST_DISPLAY_STATE_FAILED;
    s_status.updated_ms = si_monotonic_ms();
    strlcpy(s_status.error, success ? "" : (error ? error : "operation failed"),
            sizeof(s_status.error));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_manager_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create display plan mutex");
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SI_HOST_DISPLAY_STATE_EMPTY;
    return ESP_OK;
}

esp_err_t si_host_display_plan_create(
    const si_host_display_request_t *request, si_host_display_status_t *out)
{
    if (si_host_display_validate_request(request) != SI_HOST_DISPLAY_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!state_accepts_new_plan(s_status.state)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t now_ms = si_monotonic_ms();
    s_plan_seq++;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SI_HOST_DISPLAY_STATE_PLANNED;
    s_status.approval_required = true;
    s_status.reboot_required = request->persistent;
    s_status.created_ms = now_ms;
    s_status.updated_ms = now_ms;
    s_status.request = *request;
    snprintf(s_status.plan_id, sizeof(s_status.plan_id), "hd-%08lx-%04lx",
             (unsigned long)now_ms, (unsigned long)(s_plan_seq & 0xffffU));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_plan_get(si_host_display_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_plan_approve(
    const char *plan_id, const char *session_id, si_host_display_status_t *out)
{
    if (!session_id || !session_id[0] ||
        strlen(session_id) > SI_HOST_DISPLAY_SESSION_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_HOST_DISPLAY_STATE_PLANNED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_HOST_DISPLAY_STATE_APPROVED;
    s_status.approved = true;
    s_status.updated_ms = si_monotonic_ms();
    strlcpy(s_status.approved_by_session, session_id,
            sizeof(s_status.approved_by_session));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_plan_cancel(
    const char *plan_id, const char *session_id, si_host_display_status_t *out)
{
    if (!session_id || !session_id[0] ||
        strlen(session_id) > SI_HOST_DISPLAY_SESSION_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_HOST_DISPLAY_STATE_PLANNED &&
        s_status.state != SI_HOST_DISPLAY_STATE_APPROVED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_HOST_DISPLAY_STATE_CANCELLED;
    s_status.approved = false;
    s_status.updated_ms = si_monotonic_ms();
    strlcpy(s_status.approved_by_session, session_id,
            sizeof(s_status.approved_by_session));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_plan_begin_apply(
    const char *plan_id, si_host_display_status_t *out)
{
    return transition_begin(plan_id, SI_HOST_DISPLAY_STATE_APPROVED,
                            SI_HOST_DISPLAY_STATE_APPROVED,
                            SI_HOST_DISPLAY_STATE_APPLYING, out);
}

esp_err_t si_host_display_plan_finish_apply(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out)
{
    return transition_finish(plan_id, SI_HOST_DISPLAY_STATE_APPLYING,
                             SI_HOST_DISPLAY_STATE_APPLIED, success, error,
                             out);
}

esp_err_t si_host_display_plan_begin_verify(
    const char *plan_id, si_host_display_status_t *out)
{
    return transition_begin(plan_id, SI_HOST_DISPLAY_STATE_APPLIED,
                            SI_HOST_DISPLAY_STATE_FAILED,
                            SI_HOST_DISPLAY_STATE_VERIFYING, out);
}

esp_err_t si_host_display_plan_finish_verify(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out)
{
    return transition_finish(plan_id, SI_HOST_DISPLAY_STATE_VERIFYING,
                             SI_HOST_DISPLAY_STATE_VERIFIED, success, error,
                             out);
}

esp_err_t si_host_display_plan_begin_rollback(
    const char *plan_id, si_host_display_status_t *out)
{
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take display plan lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_HOST_DISPLAY_STATE_APPLIED &&
        s_status.state != SI_HOST_DISPLAY_STATE_VERIFIED &&
        s_status.state != SI_HOST_DISPLAY_STATE_FAILED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_HOST_DISPLAY_STATE_ROLLING_BACK;
    s_status.updated_ms = si_monotonic_ms();
    s_status.error[0] = '\0';
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_host_display_plan_finish_rollback(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out)
{
    return transition_finish(plan_id, SI_HOST_DISPLAY_STATE_ROLLING_BACK,
                             SI_HOST_DISPLAY_STATE_ROLLED_BACK, success, error,
                             out);
}

const char *si_host_display_state_name(si_host_display_state_t state)
{
    switch (state) {
    case SI_HOST_DISPLAY_STATE_PLANNED:
        return "planned";
    case SI_HOST_DISPLAY_STATE_APPROVED:
        return "approved";
    case SI_HOST_DISPLAY_STATE_APPLYING:
        return "applying";
    case SI_HOST_DISPLAY_STATE_APPLIED:
        return "applied";
    case SI_HOST_DISPLAY_STATE_VERIFYING:
        return "verifying";
    case SI_HOST_DISPLAY_STATE_VERIFIED:
        return "verified";
    case SI_HOST_DISPLAY_STATE_FAILED:
        return "failed";
    case SI_HOST_DISPLAY_STATE_ROLLING_BACK:
        return "rolling_back";
    case SI_HOST_DISPLAY_STATE_ROLLED_BACK:
        return "rolled_back";
    case SI_HOST_DISPLAY_STATE_CANCELLED:
        return "cancelled";
    case SI_HOST_DISPLAY_STATE_EMPTY:
    default:
        return "empty";
    }
}
