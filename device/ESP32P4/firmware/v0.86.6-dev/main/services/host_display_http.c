#include "host_display_http.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "host_display_manager.h"
#include "http_api.h"

#define HOST_DISPLAY_BODY_MAX 1024U

static void add_status_json(
    cJSON *root, const si_host_display_status_t *status)
{
    cJSON_AddStringToObject(root, "schema_version",
                            "exoanchor.host_display.plan.v1");
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddStringToObject(root, "reference_target", "ubuntu-grub-drm");
    cJSON_AddStringToObject(root, "state",
                            si_host_display_state_name(status->state));
    cJSON_AddStringToObject(root, "plan_id", status->plan_id);
    cJSON_AddBoolToObject(root, "approval_required",
                          status->approval_required);
    cJSON_AddBoolToObject(root, "approved", status->approved);
    cJSON_AddBoolToObject(root, "reboot_required",
                          status->reboot_required);
    cJSON_AddBoolToObject(root, "temporary_apply_supported", false);
    cJSON_AddBoolToObject(root, "persistent_apply_supported", true);
    cJSON_AddNumberToObject(root, "created_ms", status->created_ms);
    cJSON_AddNumberToObject(root, "updated_ms", status->updated_ms);
    if (status->approved_by_session[0]) {
        cJSON_AddStringToObject(root, "approved_by_session",
                                status->approved_by_session);
    }
    if (status->error[0]) {
        cJSON_AddStringToObject(root, "error", status->error);
    }
    cJSON *target = cJSON_AddObjectToObject(root, "target");
    cJSON_AddStringToObject(target, "connector", status->request.connector);
    cJSON_AddNumberToObject(target, "width", status->request.width);
    cJSON_AddNumberToObject(target, "height", status->request.height);
    cJSON_AddNumberToObject(
        target, "refresh_millihz", status->request.refresh_millihz);
    cJSON_AddNumberToObject(
        target, "refresh_hz",
        (double)status->request.refresh_millihz / 1000.0);
    cJSON_AddBoolToObject(target, "persistent", status->request.persistent);
}

static esp_err_t send_status(
    httpd_req_t *req, const si_host_display_status_t *status)
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
                                   "invalid display plan");
    }
    if (error == ESP_ERR_NOT_FOUND) {
        return si_http_send_text_status(req, "404 Not Found",
                                        "display plan not found");
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "display plan state conflict");
    }
    return si_http_send_text_status(req, "503 Service Unavailable",
                                    "display plan manager unavailable");
}

esp_err_t si_host_display_http_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        esp_err_t auth_ret =
            si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
        if (auth_ret != ESP_OK) {
            return auth_ret;
        }
        si_host_display_status_t status;
        esp_err_t ret = si_host_display_plan_get(&status);
        return ret == ESP_OK ? send_status(req, &status) :
               send_manager_error(req, ret);
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }

    si_auth_session_context_t session;
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SSH, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "browser session required");
    }
    if (req->content_len <= 0 ||
        req->content_len >= (int)HOST_DISPLAY_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "json body required");
    }
    char *body = calloc(1, HOST_DISPLAY_BODY_MAX);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "request alloc failed");
    }
    cJSON *root = NULL;
    esp_err_t ret =
        si_http_recv_json(req, body, HOST_DISPLAY_BODY_MAX, &root);
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
    si_host_display_status_t status;

    if (action && strcmp(action, "plan") == 0) {
        const cJSON *connector_item =
            cJSON_GetObjectItemCaseSensitive(root, "connector");
        const cJSON *width_item =
            cJSON_GetObjectItemCaseSensitive(root, "width");
        const cJSON *height_item =
            cJSON_GetObjectItemCaseSensitive(root, "height");
        const cJSON *refresh_item =
            cJSON_GetObjectItemCaseSensitive(root, "refresh_hz");
        const cJSON *persistent_item =
            cJSON_GetObjectItemCaseSensitive(root, "persistent");
        si_host_display_request_t request = {0};
        if (cJSON_IsString(connector_item)) {
            strlcpy(request.connector, connector_item->valuestring,
                    sizeof(request.connector));
        }
        if (cJSON_IsNumber(width_item) &&
            width_item->valuedouble == (double)width_item->valueint &&
            width_item->valueint >= 320 && width_item->valueint <= 7680) {
            request.width = (uint16_t)width_item->valueint;
        }
        if (cJSON_IsNumber(height_item) &&
            height_item->valuedouble == (double)height_item->valueint &&
            height_item->valueint >= 200 && height_item->valueint <= 4320) {
            request.height = (uint16_t)height_item->valueint;
        }
        if (cJSON_IsNumber(refresh_item) &&
            refresh_item->valuedouble == (double)refresh_item->valueint &&
            refresh_item->valueint >= 10 && refresh_item->valueint <= 240) {
            request.refresh_millihz =
                (uint32_t)refresh_item->valueint * 1000U;
        }
        request.persistent = cJSON_IsBool(persistent_item) &&
                             cJSON_IsTrue(persistent_item);
        ret = si_host_display_plan_create(&request, &status);
    } else if (action && strcmp(action, "approve") == 0) {
        const cJSON *approved_item =
            cJSON_GetObjectItemCaseSensitive(root, "approved");
        if (!cJSON_IsBool(approved_item) ||
            !cJSON_IsTrue(approved_item)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "approved=true required");
        }
        ret = si_host_display_plan_approve(
            plan_id, session.session_id, &status);
    } else if (action && strcmp(action, "cancel") == 0) {
        ret = si_host_display_plan_cancel(
            plan_id, session.session_id, &status);
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "action must be plan, approve, or cancel");
    }
    cJSON_Delete(root);
    return ret == ESP_OK ? send_status(req, &status) :
           send_manager_error(req, ret);
}
