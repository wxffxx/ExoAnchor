#include "boot_key_manager.h"

#include <stdio.h>
#include <string.h>

#include "control_lease.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_device.h"
#include "power_control.h"
#include "time_utils.h"

#define SI_BOOT_KEY_TASK_STACK 4096U
#define SI_BOOT_KEY_TASK_POLL_MS 20U

static const char *TAG = "si-boot-key";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static si_boot_key_status_t s_status;
static uint32_t s_plan_seq;

static esp_err_t take_lock(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE ?
               ESP_OK : ESP_ERR_TIMEOUT;
}

static bool plan_matches_locked(const char *plan_id)
{
    return plan_id && plan_id[0] &&
           strcmp(plan_id, s_status.plan_id) == 0;
}

static bool state_has_worker(si_boot_key_state_t state)
{
    return state == SI_BOOT_KEY_STATE_STARTING ||
           state == SI_BOOT_KEY_STATE_WAITING_HID ||
           state == SI_BOOT_KEY_STATE_WAITING_START ||
           state == SI_BOOT_KEY_STATE_RUNNING;
}

static bool state_accepts_new_plan(si_boot_key_state_t state)
{
    return state == SI_BOOT_KEY_STATE_EMPTY ||
           state == SI_BOOT_KEY_STATE_CANCELLED ||
           state == SI_BOOT_KEY_STATE_VERIFIED ||
           state == SI_BOOT_KEY_STATE_TIMED_OUT ||
           state == SI_BOOT_KEY_STATE_FAILED ||
           state == SI_BOOT_KEY_STATE_PREEMPTED;
}

static void copy_out_locked(si_boot_key_status_t *out)
{
    if (out) {
        *out = s_status;
    }
}

static void finish_locked(si_boot_key_state_t state, const char *error)
{
    s_status.state = state;
    s_status.finished_ms = si_monotonic_ms();
    s_status.updated_ms = s_status.finished_ms;
    strlcpy(s_status.error, error ? error : "", sizeof(s_status.error));
    s_task = NULL;
}

static bool task_snapshot(char *plan_id, size_t plan_id_size,
                          si_boot_key_sequence_config_t *config,
                          uint8_t *attempts, bool *cancel_requested)
{
    if (take_lock() != ESP_OK) {
        return false;
    }
    strlcpy(plan_id, s_status.plan_id, plan_id_size);
    *config = s_status.config;
    *attempts = s_status.attempts_sent;
    *cancel_requested = s_status.cancel_requested;
    xSemaphoreGive(s_lock);
    return true;
}

static void task_finish(const char *plan_id, si_boot_key_state_t state,
                        const char *error,
                        const si_hid_owner_token_t *hid_owner)
{
    if (take_lock() == ESP_OK) {
        if (plan_matches_locked(plan_id)) {
            finish_locked(state, error);
        }
        xSemaphoreGive(s_lock);
    }
    if (hid_owner && hid_owner->generation != 0U) {
        const si_hid_command_t release = {
            .type = SI_HID_COMMAND_RELEASE_ALL,
        };
        (void)si_hid_execute_owned(&release, hid_owner, NULL, NULL);
        (void)si_control_lease_release_embedded_owner(hid_owner);
    }
}

static void boot_key_task(void *arg)
{
    (void)arg;
    char plan_id[SI_BOOT_KEY_PLAN_ID_MAX + 1U] = {0};
    si_boot_key_sequence_config_t config = {0};
    uint8_t attempts = 0;
    bool cancel_requested = false;
    si_hid_owner_token_t hid_owner = {0};
    if (!task_snapshot(plan_id, sizeof(plan_id), &config, &attempts,
                       &cancel_requested)) {
        task_finish(plan_id, SI_BOOT_KEY_STATE_FAILED,
                    "boot-key status unavailable", &hid_owner);
        vTaskDelete(NULL);
        return;
    }
    if (cancel_requested) {
        task_finish(plan_id, SI_BOOT_KEY_STATE_CANCELLED, NULL, &hid_owner);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t lease_ret =
        si_control_lease_touch_boot_sequence_owned(&hid_owner);
    if (lease_ret != ESP_OK) {
        task_finish(plan_id, SI_BOOT_KEY_STATE_PREEMPTED,
                    "HID control lease unavailable or human KVM active",
                    &hid_owner);
        vTaskDelete(NULL);
        return;
    }

    if (config.trigger == SI_BOOT_KEY_TRIGGER_RESET) {
        if (!task_snapshot(plan_id, sizeof(plan_id), &config, &attempts,
                           &cancel_requested)) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_FAILED,
                        "boot-key status unavailable", &hid_owner);
            vTaskDelete(NULL);
            return;
        }
        if (cancel_requested) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_CANCELLED, NULL,
                        &hid_owner);
            vTaskDelete(NULL);
            return;
        }
        esp_err_t reset_ret = si_power_press_reset(0);
        if (reset_ret != ESP_OK) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_FAILED,
                        "Host reset trigger failed", &hid_owner);
            vTaskDelete(NULL);
            return;
        }
    }

    uint32_t started_ms = si_monotonic_ms();
    if (take_lock() == ESP_OK) {
        if (plan_matches_locked(plan_id)) {
            s_status.started_ms = started_ms;
            s_status.updated_ms = started_ms;
            s_status.reset_triggered =
                config.trigger == SI_BOOT_KEY_TRIGGER_RESET;
            s_status.state = SI_BOOT_KEY_STATE_WAITING_START;
        }
        xSemaphoreGive(s_lock);
    }

    while (true) {
        if (!task_snapshot(plan_id, sizeof(plan_id), &config, &attempts,
                           &cancel_requested)) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_FAILED,
                        "boot-key status unavailable", &hid_owner);
            break;
        }

        si_control_lease_status_t lease;
        si_control_lease_get_status(&lease);
        lease_ret =
            si_control_lease_touch_boot_sequence_owned(&hid_owner);
        bool lease_held =
            lease_ret == ESP_OK &&
            si_control_lease_owner_session_matches(
                "agent", "embedded-agent");
        uint32_t elapsed_ms = si_monotonic_ms() - started_ms;
        si_boot_key_decision_t decision = si_boot_key_sequence_decide(
            &config, elapsed_ms, attempts, cancel_requested,
            false, lease_held, si_hid_is_mounted());

        if (decision == SI_BOOT_KEY_DECISION_SEND) {
            si_hid_command_t command = {
                .type = SI_HID_COMMAND_COMBO,
                .keys = {config.key_code},
                .key_count = 1,
            };
            esp_err_t hid_ret = si_hid_execute_owned(
                &command, &hid_owner, NULL, NULL);
            if (hid_ret != ESP_OK) {
                task_finish(plan_id, SI_BOOT_KEY_STATE_FAILED,
                            "boot-key HID send failed", &hid_owner);
                break;
            }
            attempts++;
            if (take_lock() == ESP_OK) {
                if (plan_matches_locked(plan_id)) {
                    s_status.attempts_sent = attempts;
                    s_status.updated_ms = si_monotonic_ms();
                    s_status.state = SI_BOOT_KEY_STATE_RUNNING;
                }
                xSemaphoreGive(s_lock);
            }
        } else if (decision == SI_BOOT_KEY_DECISION_WAIT_HID ||
                   decision == SI_BOOT_KEY_DECISION_WAIT) {
            if (take_lock() == ESP_OK) {
                if (plan_matches_locked(plan_id)) {
                    s_status.state =
                        decision == SI_BOOT_KEY_DECISION_WAIT_HID ?
                            SI_BOOT_KEY_STATE_WAITING_HID :
                            SI_BOOT_KEY_STATE_WAITING_START;
                    s_status.updated_ms = si_monotonic_ms();
                }
                xSemaphoreGive(s_lock);
            }
        } else if (decision == SI_BOOT_KEY_DECISION_COMPLETE) {
            task_finish(plan_id,
                        SI_BOOT_KEY_STATE_AWAITING_VERIFICATION, NULL,
                        &hid_owner);
            break;
        } else if (decision == SI_BOOT_KEY_DECISION_CANCEL) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_CANCELLED, NULL,
                        &hid_owner);
            break;
        } else if (decision == SI_BOOT_KEY_DECISION_PREEMPT) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_PREEMPTED,
                        "human KVM activity or control lease revocation",
                        &hid_owner);
            break;
        } else if (decision == SI_BOOT_KEY_DECISION_TIMEOUT) {
            task_finish(plan_id, SI_BOOT_KEY_STATE_TIMED_OUT,
                        "boot-key sequence timed out", &hid_owner);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SI_BOOT_KEY_TASK_POLL_MS));
    }
    vTaskDelete(NULL);
}

esp_err_t si_boot_key_manager_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG,
                        "create boot-key mutex");
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SI_BOOT_KEY_STATE_EMPTY;
    return ESP_OK;
}

esp_err_t si_boot_key_plan_create(
    const si_boot_key_sequence_config_t *config, si_boot_key_status_t *out)
{
    if (!si_boot_key_sequence_validate(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    if (!state_accepts_new_plan(s_status.state)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t now_ms = si_monotonic_ms();
    s_plan_seq++;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SI_BOOT_KEY_STATE_PLANNED;
    s_status.approval_required = true;
    s_status.created_ms = now_ms;
    s_status.updated_ms = now_ms;
    s_status.config = *config;
    snprintf(s_status.plan_id, sizeof(s_status.plan_id), "bk-%08lx-%04lx",
             (unsigned long)now_ms,
             (unsigned long)(s_plan_seq & 0xffffU));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_boot_key_plan_get(si_boot_key_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_boot_key_plan_approve(
    const char *plan_id, const char *session_id, si_boot_key_status_t *out)
{
    if (!session_id || !session_id[0] ||
        strlen(session_id) > SI_BOOT_KEY_SESSION_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_BOOT_KEY_STATE_PLANNED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_BOOT_KEY_STATE_APPROVED;
    s_status.approved = true;
    s_status.updated_ms = si_monotonic_ms();
    strlcpy(s_status.approved_by_session, session_id,
            sizeof(s_status.approved_by_session));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_boot_key_plan_start(
    const char *plan_id, si_boot_key_status_t *out)
{
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_BOOT_KEY_STATE_APPROVED || !s_status.approved ||
        s_task) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_BOOT_KEY_STATE_STARTING;
    s_status.updated_ms = si_monotonic_ms();
    BaseType_t task_ok = xTaskCreate(
        boot_key_task, "si_boot_key", SI_BOOT_KEY_TASK_STACK, NULL,
        tskIDLE_PRIORITY + 3, &s_task);
    if (task_ok != pdPASS) {
        finish_locked(SI_BOOT_KEY_STATE_FAILED,
                      "create boot-key task failed");
        copy_out_locked(out);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_boot_key_plan_cancel(
    const char *plan_id, const char *session_id, si_boot_key_status_t *out)
{
    if (!session_id || !session_id[0] ||
        strlen(session_id) > SI_BOOT_KEY_SESSION_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (state_has_worker(s_status.state)) {
        s_status.cancel_requested = true;
        s_status.updated_ms = si_monotonic_ms();
    } else if (s_status.state == SI_BOOT_KEY_STATE_PLANNED ||
               s_status.state == SI_BOOT_KEY_STATE_APPROVED ||
               s_status.state ==
                   SI_BOOT_KEY_STATE_AWAITING_VERIFICATION) {
        finish_locked(SI_BOOT_KEY_STATE_CANCELLED, NULL);
        s_status.approved = false;
    } else {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t si_boot_key_plan_confirm(
    const char *plan_id, const char *session_id, const char *evidence,
    si_boot_key_status_t *out)
{
    if (!session_id || !session_id[0] ||
        strlen(session_id) > SI_BOOT_KEY_SESSION_ID_MAX ||
        !evidence || !evidence[0] ||
        strlen(evidence) > SI_BOOT_KEY_EVIDENCE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "take boot-key lock");
    if (!plan_matches_locked(plan_id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.state != SI_BOOT_KEY_STATE_AWAITING_VERIFICATION) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SI_BOOT_KEY_STATE_VERIFIED;
    s_status.updated_ms = si_monotonic_ms();
    s_status.finished_ms = s_status.updated_ms;
    strlcpy(s_status.verified_by_session, session_id,
            sizeof(s_status.verified_by_session));
    strlcpy(s_status.evidence, evidence, sizeof(s_status.evidence));
    copy_out_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool si_boot_key_cancel_for_manual_hid(void)
{
    bool cancelled = false;
    if (take_lock() != ESP_OK) {
        return false;
    }
    if (state_has_worker(s_status.state)) {
        s_status.cancel_requested = true;
        s_status.updated_ms = si_monotonic_ms();
        cancelled = true;
    } else if (s_status.state == SI_BOOT_KEY_STATE_PLANNED ||
               s_status.state == SI_BOOT_KEY_STATE_APPROVED) {
        s_status.cancel_requested = true;
        finish_locked(SI_BOOT_KEY_STATE_CANCELLED,
                      "cancelled by manual HID input");
        cancelled = true;
    }
    xSemaphoreGive(s_lock);
    return cancelled;
}

const char *si_boot_key_state_name(si_boot_key_state_t state)
{
    switch (state) {
    case SI_BOOT_KEY_STATE_EMPTY:
        return "empty";
    case SI_BOOT_KEY_STATE_PLANNED:
        return "planned";
    case SI_BOOT_KEY_STATE_APPROVED:
        return "approved";
    case SI_BOOT_KEY_STATE_STARTING:
        return "starting";
    case SI_BOOT_KEY_STATE_WAITING_HID:
        return "waiting_hid";
    case SI_BOOT_KEY_STATE_WAITING_START:
        return "waiting_start";
    case SI_BOOT_KEY_STATE_RUNNING:
        return "running";
    case SI_BOOT_KEY_STATE_AWAITING_VERIFICATION:
        return "awaiting_verification";
    case SI_BOOT_KEY_STATE_VERIFIED:
        return "verified";
    case SI_BOOT_KEY_STATE_CANCELLED:
        return "cancelled";
    case SI_BOOT_KEY_STATE_PREEMPTED:
        return "preempted";
    case SI_BOOT_KEY_STATE_TIMED_OUT:
        return "timed_out";
    case SI_BOOT_KEY_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
