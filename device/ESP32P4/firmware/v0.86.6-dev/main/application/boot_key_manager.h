#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot_key_sequence.h"
#include "esp_err.h"

/*
 * Deterministic Host firmware-entry sequencer.
 *
 * Ownership: one in-memory, exact-approved plan and one worker task.
 * Concurrency: status and cancellation are protected by one mutex. The worker
 * renews the Embedded Agent HID lease and stops on human KVM activity.
 * Recovery: every exit releases all HID keys and the lease. Sending all keys
 * only reaches awaiting_verification; success requires separate human evidence.
 */

#define SI_BOOT_KEY_PLAN_ID_MAX 31U
#define SI_BOOT_KEY_SESSION_ID_MAX 31U
#define SI_BOOT_KEY_EVIDENCE_MAX 95U
#define SI_BOOT_KEY_ERROR_MAX 127U

typedef enum {
    SI_BOOT_KEY_STATE_EMPTY = 0,
    SI_BOOT_KEY_STATE_PLANNED,
    SI_BOOT_KEY_STATE_APPROVED,
    SI_BOOT_KEY_STATE_STARTING,
    SI_BOOT_KEY_STATE_WAITING_HID,
    SI_BOOT_KEY_STATE_WAITING_START,
    SI_BOOT_KEY_STATE_RUNNING,
    SI_BOOT_KEY_STATE_AWAITING_VERIFICATION,
    SI_BOOT_KEY_STATE_VERIFIED,
    SI_BOOT_KEY_STATE_CANCELLED,
    SI_BOOT_KEY_STATE_PREEMPTED,
    SI_BOOT_KEY_STATE_TIMED_OUT,
    SI_BOOT_KEY_STATE_FAILED,
} si_boot_key_state_t;

typedef struct {
    si_boot_key_state_t state;
    bool approval_required;
    bool approved;
    bool cancel_requested;
    bool reset_triggered;
    uint8_t attempts_sent;
    uint32_t created_ms;
    uint32_t started_ms;
    uint32_t updated_ms;
    uint32_t finished_ms;
    char plan_id[SI_BOOT_KEY_PLAN_ID_MAX + 1U];
    char approved_by_session[SI_BOOT_KEY_SESSION_ID_MAX + 1U];
    char verified_by_session[SI_BOOT_KEY_SESSION_ID_MAX + 1U];
    char evidence[SI_BOOT_KEY_EVIDENCE_MAX + 1U];
    char error[SI_BOOT_KEY_ERROR_MAX + 1U];
    si_boot_key_sequence_config_t config;
} si_boot_key_status_t;

esp_err_t si_boot_key_manager_start(void);
esp_err_t si_boot_key_plan_create(
    const si_boot_key_sequence_config_t *config, si_boot_key_status_t *out);
esp_err_t si_boot_key_plan_get(si_boot_key_status_t *out);
esp_err_t si_boot_key_plan_approve(
    const char *plan_id, const char *session_id, si_boot_key_status_t *out);
esp_err_t si_boot_key_plan_start(
    const char *plan_id, si_boot_key_status_t *out);
esp_err_t si_boot_key_plan_cancel(
    const char *plan_id, const char *session_id, si_boot_key_status_t *out);
esp_err_t si_boot_key_plan_confirm(
    const char *plan_id, const char *session_id, const char *evidence,
    si_boot_key_status_t *out);
bool si_boot_key_cancel_for_manual_hid(void);

const char *si_boot_key_state_name(si_boot_key_state_t state);
