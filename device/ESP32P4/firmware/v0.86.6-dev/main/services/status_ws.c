// Overall status HTTP and HID WebSocket adapters.
#include "status_ws.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth_service.h"
#include "boot_key_manager.h"
#include "cJSON.h"
#include "control_lease.h"
#include "device_http.h"
#include "device_observation_service.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hid_json.h"
#include "http_api.h"
#include "web_server.h"

#define HID_WS_MAX_FRAME 1024

static const char *TAG = "si-status-ws";

typedef struct {
    uint32_t stream_id;
    uint32_t stream_owner_epoch;
    uint32_t auth_generation;
    char session_id[SI_AUTH_SESSION_ID_MAX_LEN + 1];
    si_hid_owner_token_t owner;
} hid_ws_ctx_t;

static void hid_ws_ctx_free(void *arg)
{
    hid_ws_ctx_t *ctx = (hid_ws_ctx_t *)arg;
    if (!ctx) {
        return;
    }
    /* Server-issued generation, not stream_id, is the release capability. */
    (void)si_hid_owner_release_if_current(&ctx->owner);
    free(ctx);
}

static void *status_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : calloc(count, size);
}

static esp_err_t hid_ws_stream_id(httpd_req_t *req, uint32_t *stream_id,
                                  bool *present)
{
    if (!req || !stream_id || !present) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream_id = 0U;
    *present = false;
    char query[96] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "stream_id", value,
                              sizeof(value)) != ESP_OK) {
        return ESP_OK;
    }
    *present = true;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream_id = (uint32_t)parsed;
    return ESP_OK;
}

static bool hid_ws_handshake_auth_guard(void *opaque)
{
    const si_auth_session_context_t *expected = opaque;
    si_auth_session_context_t live = {0};
    return expected && si_auth_session_get_live_context(
        expected->session_id, expected->generation, &live) &&
        live.authenticated && live.principal == SI_PRINCIPAL_BROWSER &&
        live.generation == expected->generation &&
        strcmp(live.session_id, expected->session_id) == 0 &&
        si_capability_set_has(live.capabilities, SI_CAPABILITY_HID);
}

static bool hid_ws_live_guard(void *opaque)
{
    hid_ws_ctx_t *ctx = opaque;
    si_auth_session_context_t expected = {0};
    if (!ctx) return false;
    expected.authenticated = true;
    expected.principal = SI_PRINCIPAL_BROWSER;
    expected.capabilities = SI_CAPABILITY_HID;
    expected.generation = ctx->auth_generation;
    strlcpy(expected.session_id, ctx->session_id,
            sizeof(expected.session_id));
    return hid_ws_handshake_auth_guard(&expected) &&
        si_control_lease_kvm_hid_owner_is_current(
            ctx->stream_id, ctx->stream_owner_epoch, ctx->session_id,
            ctx->auth_generation);
}

static esp_err_t hid_ws_reject_after_send(esp_err_t send_ret)
{
    /* A pre-handshake denial must remain a failed callback even when its HTTP
     * error body was transmitted successfully.  Returning ESP_OK here would
     * make ESP-IDF continue with the 101 upgrade without a session context. */
    return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;
}

esp_err_t overall_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_device_network_observation_t network_status;
    si_device_video_observation_t video_status;
    si_device_video_control_observation_t video_control;
    si_device_video_mode_observation_t *video_modes =
        status_calloc(SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES,
                      sizeof(*video_modes));
    size_t video_mode_count = 0;
    si_device_hid_observation_t hid_status;
    si_device_control_lease_observation_t control_lease_status;
    si_device_power_observation_t power_status;
    si_device_ms2109_power_observation_t ms2109_power_status;
    si_device_performance_observation_t performance_status;
    si_device_observation_get_network(&network_status);
    si_device_observation_get_video(&video_status, &video_control, video_modes,
                                    SI_DEVICE_OBSERVATION_MAX_VIDEO_MODES,
                                    &video_mode_count);
    si_device_observation_get_hid(&hid_status);
    si_device_observation_get_control_lease(&control_lease_status);
    si_device_observation_get_power(&power_status);
    si_device_observation_get_ms2109_power(&ms2109_power_status);
    si_device_observation_get_performance(&performance_status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "server_uptime",
                            si_device_observation_uptime_seconds());
    cJSON *video = cJSON_AddObjectToObject(root, "video");
    si_device_http_add_video_json(video, &video_status, &video_control,
                                  video_modes, video_mode_count);
    free(video_modes);
    cJSON *hid = cJSON_AddObjectToObject(root, "hid");
    si_device_http_add_hid_json(hid, &hid_status);
    cJSON *control_lease = cJSON_AddObjectToObject(root, "control_lease");
    si_device_http_add_control_lease_json(control_lease, &control_lease_status);
    cJSON *power = cJSON_AddObjectToObject(root, "power");
    si_device_http_add_power_json(power, &power_status);
    cJSON *ms2109 = cJSON_AddObjectToObject(root, "ms2109");
    si_device_http_add_ms2109_power_json(ms2109, &ms2109_power_status);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    si_device_http_add_network_json(network, &network_status);

    cJSON_AddNumberToObject(root, "active_connections", si_web_client_count());
    cJSON_AddBoolToObject(root, "authEnabled", si_auth_is_enabled());
    cJSON *performance = cJSON_AddObjectToObject(root, "performance");
    si_device_http_add_performance_json(performance, &performance_status,
                                        si_web_client_count());

    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

#ifdef CONFIG_HTTPD_WS_SUPPORT
esp_err_t hid_ws_pre_handshake(httpd_req_t *req)
{
    uint32_t stream_id = 0U;
    bool stream_id_present = false;
    if (hid_ws_stream_id(req, &stream_id, &stream_id_present) != ESP_OK) {
        return hid_ws_reject_after_send(httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST, "invalid stream_id"));
    }
    if (!stream_id_present) {
        return hid_ws_reject_after_send(httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "stream_id is required for revocable KVM HID"));
    }
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_HID, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return hid_ws_reject_after_send(httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN, "KVM HID requires a browser session"));
    }
    /* Opening or refreshing the KVM page must remain a passive observation.
     * Do not let its eager HID WebSocket replace a live Agent/MCP input lease;
     * the page first exposes an explicit human-stop action, then reconnects. */
    si_control_lease_status_t lease = {0};
    si_control_lease_get_status(&lease);
    if (lease.input_control_active) {
        return hid_ws_reject_after_send(si_http_send_text_status(
            req, "409 Conflict", "automated input control is active"));
    }
    si_hid_owner_token_t owner = {0};
    if (!si_control_lease_claim_kvm_hid_owner(
            stream_id, session.session_id, session.generation,
            hid_ws_handshake_auth_guard, &session, &owner)) {
        return hid_ws_reject_after_send(si_http_send_text_status(
            req, "409 Conflict",
            "stale KVM stream or agent input control is active"));
    }
    hid_ws_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->stream_id = stream_id;
    ctx->stream_owner_epoch = owner.claim.authority_epoch;
    ctx->auth_generation = session.generation;
    strlcpy(ctx->session_id, session.session_id, sizeof(ctx->session_id));
    ctx->owner = owner;
    req->sess_ctx = ctx;
    req->free_ctx = hid_ws_ctx_free;
    si_web_log("INFO", "KVM HID WebSocket connected");
    return ESP_OK;
}

esp_err_t hid_ws_handler(httpd_req_t *req)
{
    hid_ws_ctx_t *ctx = (hid_ws_ctx_t *)req->sess_ctx;
    if (!ctx) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        hid_ws_ctx_free(ctx);
        req->sess_ctx = NULL;
        req->free_ctx = NULL;
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > HID_WS_MAX_FRAME) {
        ESP_LOGW(TAG, "HID WebSocket frame too large: %u", (unsigned)frame.len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        cJSON *root = cJSON_Parse((const char *)buf);
        if (root) {
            esp_err_t hid_ret = si_hid_json_execute_owned(
                root, &ctx->owner, hid_ws_live_guard, ctx);
            if (hid_ret == ESP_OK) {
                (void)si_boot_key_cancel_for_manual_hid();
            }
            if (hid_ret == ESP_ERR_INVALID_STATE) {
                (void)si_hid_owner_release_if_current(&ctx->owner);
                cJSON_Delete(root);
                free(buf);
                return ESP_ERR_INVALID_STATE;
            }
            if (hid_ret != ESP_OK && hid_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "HID command failed: %s", esp_err_to_name(hid_ret));
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Invalid HID WebSocket JSON");
        }
    }
    free(buf);
    return ret;
}
#endif
