#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "host_display_mode.h"

/*
 * Single-device Host display plan manager.
 *
 * Ownership: owns one in-memory plan, its exact browser approval, transition
 * timestamps, and last error. Commands and SSH transport remain outside.
 * Concurrency: every read/write is protected by one mutex; no worker task.
 * Recovery: plan survives browser disconnects but not device reboot. A reboot
 * never implies approval; persistent host rollback material lives on the Host.
 */

#define SI_HOST_DISPLAY_SESSION_ID_MAX 31U
#define SI_HOST_DISPLAY_ERROR_MAX 127U

typedef enum {
    SI_HOST_DISPLAY_STATE_EMPTY = 0,
    SI_HOST_DISPLAY_STATE_PLANNED,
    SI_HOST_DISPLAY_STATE_APPROVED,
    SI_HOST_DISPLAY_STATE_APPLYING,
    SI_HOST_DISPLAY_STATE_APPLIED,
    SI_HOST_DISPLAY_STATE_VERIFYING,
    SI_HOST_DISPLAY_STATE_VERIFIED,
    SI_HOST_DISPLAY_STATE_FAILED,
    SI_HOST_DISPLAY_STATE_ROLLING_BACK,
    SI_HOST_DISPLAY_STATE_ROLLED_BACK,
    SI_HOST_DISPLAY_STATE_CANCELLED,
} si_host_display_state_t;

typedef struct {
    si_host_display_state_t state;
    bool approval_required;
    bool approved;
    bool reboot_required;
    uint32_t created_ms;
    uint32_t updated_ms;
    char plan_id[SI_HOST_DISPLAY_PLAN_ID_MAX + 1U];
    char approved_by_session[SI_HOST_DISPLAY_SESSION_ID_MAX + 1U];
    char error[SI_HOST_DISPLAY_ERROR_MAX + 1U];
    si_host_display_request_t request;
} si_host_display_status_t;

esp_err_t si_host_display_manager_start(void);
esp_err_t si_host_display_plan_create(
    const si_host_display_request_t *request, si_host_display_status_t *out);
esp_err_t si_host_display_plan_get(si_host_display_status_t *out);
esp_err_t si_host_display_plan_approve(
    const char *plan_id, const char *session_id, si_host_display_status_t *out);
esp_err_t si_host_display_plan_cancel(
    const char *plan_id, const char *session_id, si_host_display_status_t *out);

esp_err_t si_host_display_plan_begin_apply(
    const char *plan_id, si_host_display_status_t *out);
esp_err_t si_host_display_plan_finish_apply(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out);
esp_err_t si_host_display_plan_begin_verify(
    const char *plan_id, si_host_display_status_t *out);
esp_err_t si_host_display_plan_finish_verify(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out);
esp_err_t si_host_display_plan_begin_rollback(
    const char *plan_id, si_host_display_status_t *out);
esp_err_t si_host_display_plan_finish_rollback(
    const char *plan_id, bool success, const char *error,
    si_host_display_status_t *out);

const char *si_host_display_state_name(si_host_display_state_t state);
