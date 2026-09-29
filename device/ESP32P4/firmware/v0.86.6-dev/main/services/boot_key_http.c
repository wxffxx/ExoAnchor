#include "boot_key_http.h"

#include <stdlib.h>
#include <string.h>

#include "boot_key_manager.h"
#include "cJSON.h"
#include "control_lease.h"
#include "http_api.h"
#include "time_utils.h"

#define BOOT_KEY_BODY_MAX 1024U

static void add_status_json(cJSON *root, const si_boot_key_status_t *status)
{
    uint32_t remaining_ms = 0;
    if (status->config.total_timeout_ms > 0) {
        if (status->state == SI_BOOT_KEY_STATE_PLANNED ||
            status->state == SI_BOOT_KEY_STATE_APPROVED) {
            remaining_ms = status->config.total_timeout_ms;
        } else if (status->state == SI_BOOT_KEY_STATE_STARTING ||
                   status->state == SI_BOOT_KEY_STATE_WAITING_HID ||
                   status->state == SI_BOOT_KEY_STATE_WAITING_START ||
                   status->state == SI_BOOT_KEY_STATE_RUNNING) {
            uint32_t elapsed_ms = status->started_ms > 0
                                      ? si_monotonic_ms() - status->started_ms
                                      : 0;
            remaining_ms = elapsed_ms < status->config.total_timeout_ms
                               ? status->config.total_timeout_ms - elapsed_ms
                               : 0;
        }
    }
    cJSON_AddStringToObject(root, "schema_version",
                            "exoanchor.boot_key.plan.v1");
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddStringToObject(root, "state",
                            si_boot_key_state_name(status->state));
    cJSON_AddStringToObject(root, "plan_id", status->plan_id);
    cJSON_AddBoolToObject(root, "approval_required",
                          status->approval_required);
    cJSON_AddBoolToObject(root, "approved", status->approved);
    cJSON_AddBoolToObject(root, "cancel_requested",
                          status->cancel_requested);
    cJSON_AddBoolToObject(root, "reset_triggered",
                          status->reset_triggered);
    cJSON_AddNumberToObject(root, "attempts_sent",
                            status->attempts_sent);
    cJSON_AddNumberToObject(root, "created_ms", status->created_ms);
    cJSON_AddNumberToObject(root, "started_ms", status->started_ms);
    cJSON_AddNumberToObject(root, "updated_ms", status->updated_ms);
    cJSON_AddNumberToObject(root, "finished_ms", status->finished_ms);
    cJSON_AddNumberToObject(root, "remaining_ms", remaining_ms);
    cJSON_AddStringToObject(root, "verification_mode", "human_kvm");
    cJSON_AddBoolToObject(root, "agent_navigation_supported", false);
    if (status->approved_by_session[0]) {
        cJSON_AddStringToObject(root, "approved_by_session",
                                status->approved_by_session);
    }
    if (status->verified_by_session[0]) {
        cJSON_AddStringToObject(root, "verified_by_session",
                                status->verified_by_session);
    }
    if (status->evidence[0]) {
        cJSON_AddStringToObject(root, "evidence", status->evidence);
    }
    if (status->error[0]) {
        cJSON_AddStringToObject(root, "error", status->error);
    }

    cJSON *profile = cJSON_AddObjectToObject(root, "profile");
    cJSON_AddStringToObject(profile, "profile_id",
                            status->config.profile_id);
    cJSON_AddStringToObject(profile, "target",
                            si_boot_key_target_name(status->config.target));
    cJSON_AddStringToObject(profile, "trigger",
                            si_boot_key_trigger_name(status->config.trigger));
    cJSON_AddStringToObject(profile, "key_code",
                            status->config.key_code);
    cJSON_AddNumberToObject(profile, "start_delay_ms",
                            status->config.start_delay_ms);
    cJSON_AddNumberToObject(profile, "interval_ms",
                            status->config.interval_ms);
    cJSON_AddNumberToObject(profile, "max_attempts",
                            status->config.max_attempts);
    cJSON_AddNumberToObject(profile, "total_timeout_ms",
                            status->config.total_timeout_ms);
}

static esp_err_t send_status(httpd_req_t *req,
                             const si_boot_key_status_t *status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "json alloc failed");
    }
    add_status_json(root, status);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t send_manager_error(httpd_req_t *req, esp_err_t error)
{
    if (error == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid boot-key plan");
    }
    if (error == ESP_ERR_NOT_FOUND) {
        return si_http_send_text_status(req, "404 Not Found",
                                        "boot-key plan not found");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "boot-key plan state conflict");
    }
    return si_http_send_text_status(req, "503 Service Unavailable",
                                    "boot-key manager unavailable");
}

static bool json_uint_in_range(const cJSON *root, const char *name,
                               uint32_t min, uint32_t max,
                               uint32_t *value_out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) ||
        item->valuedouble != (double)item->valueint ||
        item->valueint < 0 ||
        (uint32_t)item->valueint < min ||
        (uint32_t)item->valueint > max) {
        return false;
    }
    *value_out = (uint32_t)item->valueint;
    return true;
}

static si_boot_key_target_t parse_target(const char *value)
{
    if (value && strcmp(value, "bios_setup") == 0) {
        return SI_BOOT_KEY_TARGET_BIOS_SETUP;
    }
    if (value && strcmp(value, "boot_menu") == 0) {
        return SI_BOOT_KEY_TARGET_BOOT_MENU;
    }
    return SI_BOOT_KEY_TARGET_INVALID;
}

static si_boot_key_trigger_t parse_trigger(const char *value)
{
    if (value && strcmp(value, "none") == 0) {
        return SI_BOOT_KEY_TRIGGER_NONE;
    }
    if (value && strcmp(value, "reset") == 0) {
        return SI_BOOT_KEY_TRIGGER_RESET;
    }
    return (si_boot_key_trigger_t)-1;
}

esp_err_t si_boot_key_http_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        esp_err_t auth_ret =
            si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
        if (auth_ret != ESP_OK) {
            return auth_ret;
        }
        si_boot_key_status_t status;
        esp_err_t ret = si_boot_key_plan_get(&status);
        return ret == ESP_OK ? send_status(req, &status) :
                               send_manager_error(req, ret);
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }

    si_auth_session_context_t session;
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_HID, "boot_key_sequence",
        SI_AUTHZ_RISK_HIGH, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "browser session required");
    }
    if (req->content_len <= 0 ||
        req->content_len >= (int)BOOT_KEY_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "json body required");
    }

    char *body = calloc(1, BOOT_KEY_BODY_MAX);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret =
        si_http_recv_json(req, body, BOOT_KEY_BODY_MAX, &root);
    free(body);
    if (ret != ESP_OK) {
        return ret;
    }

    const cJSON *action_item =
        cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action =
        cJSON_IsString(action_item) ? action_item->valuestring : NULL;
    const cJSON *plan_id_item =
        cJSON_GetObjectItemCaseSensitive(root, "plan_id");
    const char *plan_id =
        cJSON_IsString(plan_id_item) ? plan_id_item->valuestring : "";
    si_boot_key_status_t status;

    if (action && strcmp(action, "plan") == 0) {
        const cJSON *profile_item =
            cJSON_GetObjectItemCaseSensitive(root, "profile_id");
        const cJSON *target_item =
            cJSON_GetObjectItemCaseSensitive(root, "target");
        const cJSON *trigger_item =
            cJSON_GetObjectItemCaseSensitive(root, "trigger");
        const cJSON *key_item =
            cJSON_GetObjectItemCaseSensitive(root, "key_code");
        si_boot_key_sequence_config_t config = {0};
        if (cJSON_IsString(profile_item)) {
            strlcpy(config.profile_id, profile_item->valuestring,
                    sizeof(config.profile_id));
        }
        if (cJSON_IsString(key_item)) {
            strlcpy(config.key_code, key_item->valuestring,
                    sizeof(config.key_code));
        }
        config.target = parse_target(
            cJSON_IsString(target_item) ? target_item->valuestring : NULL);
        config.trigger = parse_trigger(
            cJSON_IsString(trigger_item) ? trigger_item->valuestring : NULL);
        uint32_t max_attempts = 0;
        bool timing_valid =
            json_uint_in_range(root, "start_delay_ms", 0, 10000,
                               &config.start_delay_ms) &&
            json_uint_in_range(root, "interval_ms", 100, 5000,
                               &config.interval_ms) &&
            json_uint_in_range(root, "max_attempts", 1, 20,
                               &max_attempts) &&
            json_uint_in_range(root, "total_timeout_ms", 1000, 60000,
                               &config.total_timeout_ms);
        config.max_attempts = timing_valid ? (uint8_t)max_attempts : 0U;
        if (config.trigger == SI_BOOT_KEY_TRIGGER_RESET) {
            auth_ret = si_http_require_capability(
                req, SI_CAPABILITY_POWER, NULL);
            if (auth_ret != ESP_OK) {
                cJSON_Delete(root);
                return auth_ret;
            }
        }
        ret = timing_valid ?
                  si_boot_key_plan_create(&config, &status) :
                  ESP_ERR_INVALID_ARG;
    } else if (action && strcmp(action, "approve") == 0) {
        const cJSON *approved =
            cJSON_GetObjectItemCaseSensitive(root, "approved");
        ret = cJSON_IsBool(approved) && cJSON_IsTrue(approved) ?
                  si_boot_key_plan_approve(
                      plan_id, session.session_id, &status) :
                  ESP_ERR_INVALID_ARG;
    } else if (action && strcmp(action, "start") == 0) {
        const cJSON *start =
            cJSON_GetObjectItemCaseSensitive(root, "start");
        ret = cJSON_IsBool(start) && cJSON_IsTrue(start) ?
                  si_boot_key_plan_start(plan_id, &status) :
                  ESP_ERR_INVALID_ARG;
    } else if (action && strcmp(action, "cancel") == 0) {
        ret = si_boot_key_plan_cancel(
            plan_id, session.session_id, &status);
    } else if (action && strcmp(action, "confirm") == 0) {
        const cJSON *confirmed =
            cJSON_GetObjectItemCaseSensitive(root, "confirmed");
        const cJSON *evidence_source =
            cJSON_GetObjectItemCaseSensitive(root, "evidence_source");
        const cJSON *evidence =
            cJSON_GetObjectItemCaseSensitive(root, "evidence");
        si_control_lease_status_t lease;
        si_control_lease_get_status(&lease);
        if (!cJSON_IsBool(confirmed) || !cJSON_IsTrue(confirmed) ||
            !cJSON_IsString(evidence_source) ||
            strcmp(evidence_source->valuestring, "human_kvm") != 0 ||
            !cJSON_IsString(evidence) || !lease.kvm_active) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            ret = si_boot_key_plan_confirm(
                plan_id, session.session_id, evidence->valuestring,
                &status);
        }
    } else {
        ret = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return ret == ESP_OK ? send_status(req, &status) :
                           send_manager_error(req, ret);
}
