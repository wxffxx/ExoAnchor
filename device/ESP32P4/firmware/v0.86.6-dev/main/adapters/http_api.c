#include "http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agent_tools_settings.h"
#include "auth_service.h"
#include "control_lease.h"
#include "device_observation_service.h"
#include "device_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "secret_store.h"

static const char *TAG = "si-http";
#define SI_HTTP_SESSION_COOKIE_MAX_AGE_SECONDS 86400U
static bool s_auth_status_failure_active;
static int64_t s_auth_status_last_log_us;

static bool auth_status_request(const httpd_req_t *req)
{
    return req && strcmp(req->uri, "/api/auth/status") == 0;
}

static void auth_status_log_failure(httpd_req_t *req, const char *reason,
                                    size_t cookie_len,
                                    size_t candidate_count)
{
    if (!auth_status_request(req)) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_auth_status_failure_active &&
        now_us - s_auth_status_last_log_us < 5000000LL) {
        return;
    }
    char message[128];
    snprintf(message, sizeof(message),
             "Web auth rejected: %s, cookie=%uB, candidates=%u",
             reason ? reason : "unknown", (unsigned)cookie_len,
             (unsigned)candidate_count);
    ESP_LOGW(TAG, "%s", message);
    si_device_observation_log_append("WARN", message);
    s_auth_status_failure_active = true;
    s_auth_status_last_log_us = now_us;
}

static void auth_status_log_recovery(httpd_req_t *req)
{
    if (!auth_status_request(req) || !s_auth_status_failure_active) {
        return;
    }
    ESP_LOGI(TAG, "Web auth recovered");
    si_device_observation_log_append("INFO", "Web auth recovered");
    s_auth_status_failure_active = false;
}

static bool request_header_copy(httpd_req_t *req, const char *name,
                                char *out, size_t out_size,
                                size_t *length_out,
                                const char **reason_out)
{
    if (!req || !name || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (length_out) {
        *length_out = 0;
    }
    if (reason_out) {
        *reason_out = "missing-token";
    }
    size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length_out) {
        *length_out = length;
    }
    if (length == 0) {
        return false;
    }
    if (length > CONFIG_HTTPD_MAX_REQ_HDR_LEN || length >= out_size) {
        if (reason_out) {
            *reason_out = "header-too-long";
        }
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, name, out, out_size) != ESP_OK) {
        out[0] = '\0';
        if (reason_out) {
            *reason_out = "header-read-failed";
        }
        return false;
    }
    if (reason_out) {
        *reason_out = NULL;
    }
    return true;
}

static bool copy_bearer_token(httpd_req_t *req, char *out, size_t out_size)
{
    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth, sizeof(auth)) != ESP_OK) {
        return false;
    }
    const char *prefix = "Bearer ";
    size_t prefix_len = strlen(prefix);
    size_t token_len = strlen(auth);
    if (strncmp(auth, prefix, prefix_len) != 0 ||
        token_len <= prefix_len ||
        token_len - prefix_len >= out_size) {
        return false;
    }
    strlcpy(out, auth + prefix_len, out_size);
    return true;
}

static bool copy_next_session_cookie(const char *cookie, size_t *offset,
                                     char *out, size_t out_size)
{
    if (!cookie || !offset || !out || out_size == 0) {
        return false;
    }
    const char *name = "EA_SESSION=";
    size_t name_len = strlen(name);
    size_t cookie_len = strlen(cookie);
    const char *cursor = cookie + *offset;
    while ((size_t)(cursor - cookie) < cookie_len) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') {
            cursor++;
        }
        const char *end = strchr(cursor, ';');
        if (!end) {
            end = cookie + cookie_len;
        }
        if ((size_t)(end - cursor) > name_len &&
            strncmp(cursor, name, name_len) == 0) {
            const char *value = cursor + name_len;
            size_t value_len = (size_t)(end - value);
            while (value_len > 0 &&
                   (value[value_len - 1] == ' ' ||
                    value[value_len - 1] == '\t')) {
                value_len--;
            }
            *offset = (size_t)(end - cookie);
            if (*end == ';') {
                (*offset)++;
            }
            if (value_len > 0 && value_len < out_size) {
                memcpy(out, value, value_len);
                out[value_len] = '\0';
                return true;
            }
        }
        cursor = *end == ';' ? end + 1 : end;
        *offset = (size_t)(cursor - cookie);
    }
    return false;
}

static bool request_uri_starts_with(const httpd_req_t *req,
                                    const char *prefix)
{
    return req && prefix &&
           strncmp(req->uri, prefix, strlen(prefix)) == 0;
}

static const char *mcp_policy_tool_for_request(const httpd_req_t *req)
{
    if (!req) {
        return NULL;
    }
    if (request_uri_starts_with(req, "/api/status") ||
        request_uri_starts_with(req, "/api/v1/network/") ||
        request_uri_starts_with(req, "/api/system/info") ||
        request_uri_starts_with(req, "/api/system/logs") ||
        request_uri_starts_with(req, "/api/video/status") ||
        request_uri_starts_with(req, "/api/hid/status") ||
        request_uri_starts_with(req, "/api/power/status") ||
        request_uri_starts_with(req, "/api/ssh/status")) {
        return "observe_status";
    }
    if (request_uri_starts_with(req, "/api/snapshot") ||
        request_uri_starts_with(req, "/api/stream")) {
        return "observe_screenshot";
    }
    if (request_uri_starts_with(req, "/api/video/lease")) {
        return "video_lease";
    }
    if (request_uri_starts_with(req, "/api/control/lease")) {
        return req->method == HTTP_GET ? "observe_status" :
                                         "control_lease";
    }
    if (request_uri_starts_with(req, "/api/hid/actions")) {
        return "hid_actions";
    }
    if (request_uri_starts_with(req, "/api/console/login")) {
        return "console_login";
    }
    if (request_uri_starts_with(req, "/api/power/action")) {
        return "power_action";
    }
    if (request_uri_starts_with(req, "/api/ssh/exec") ||
        request_uri_starts_with(req, "/api/ssh/configure-from-console") ||
        request_uri_starts_with(req, "/api/ws/ssh")) {
        return "ssh_exec";
    }
    if (request_uri_starts_with(req, "/api/uart/status")) {
        return "uart_status";
    }
    if (request_uri_starts_with(req, "/api/uart/read")) {
        return "uart_read";
    }
    if (request_uri_starts_with(req, "/api/uart/write")) {
        return "uart_write";
    }
    if (request_uri_starts_with(req, "/api/uart/authenticate")) {
        return "uart_auth";
    }
    if (request_uri_starts_with(req, "/api/uart/baud")) {
        return "uart_baud";
    }
    return NULL;
}

static const char *mcp_request_denial_reason(
    httpd_req_t *req, const si_auth_session_context_t *context)
{
    if (!context || context->principal != SI_PRINCIPAL_MCP) {
        return NULL;
    }
    if (!si_mcp_is_enabled() &&
        !request_uri_starts_with(req, "/api/settings/mcp")) {
        return "mcp disabled in settings";
    }
    if (request_uri_starts_with(req, "/api/auth/") ||
        request_uri_starts_with(req, "/api/settings/mcp") ||
        request_uri_starts_with(req, "/api/capabilities")) {
        return NULL;
    }
    const char *tool_name = mcp_policy_tool_for_request(req);
    if (!tool_name) {
        return "MCP endpoint is not mapped to a managed tool";
    }
    if (!si_mcp_tool_policy_allows(tool_name, NULL, 0)) {
        return "tool disabled for MCP by policy";
    }
    return NULL;
}

static bool mcp_request_may_pass(
    httpd_req_t *req, const si_auth_session_context_t *context)
{
    if (!context || context->principal != SI_PRINCIPAL_MCP) {
        return true;
    }
    return mcp_request_denial_reason(req, context) == NULL;
}

static bool bootstrap_request_may_pass(
    const httpd_req_t *req, const si_auth_session_context_t *context)
{
    si_auth_status_t status;
    si_auth_get_status(&status);
    if (!status.using_default) {
        return true;
    }
    if (!req || !context || context->principal != SI_PRINCIPAL_BROWSER) {
        return false;
    }
    return strcmp(req->uri, "/api/auth/status") == 0 ||
           strcmp(req->uri, "/api/auth/logout") == 0 ||
           strcmp(req->uri, "/api/settings/account") == 0 ||
           strcmp(req->uri, "/api/settings/password") == 0;
}

void si_http_set_security_headers(httpd_req_t *req)
{
    if (!req) {
        return;
    }
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        req, "Content-Security-Policy",
        "default-src 'self'; base-uri 'none'; frame-ancestors 'none'; "
        "object-src 'none'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; img-src 'self' data: blob: http: https:; "
        "media-src 'self' blob:; "
        "connect-src 'self' ws: wss: http: https:");
}

esp_err_t si_http_send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "json alloc failed");
    }
    si_http_set_security_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

esp_err_t si_http_send_text_status(httpd_req_t *req, const char *status,
                                   const char *message)
{
    si_http_set_security_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message ? message : status);
}

esp_err_t si_http_recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (!req || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received,
                                 req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

esp_err_t si_http_recv_json(httpd_req_t *req, char *buf, size_t buf_size,
                            cJSON **out_root)
{
    if (!out_root) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = NULL;

    esp_err_t body_ret = si_http_recv_body(req, buf, buf_size);
    if (body_ret == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "request body too large");
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid json");
    }
    *out_root = root;
    return ESP_OK;
}

bool si_http_get_auth_token(httpd_req_t *req, char *out, size_t out_size)
{
    if (!req || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (copy_bearer_token(req, out, out_size)) {
        return true;
    }

    size_t cookie_len = 0;
    char cookie[CONFIG_HTTPD_MAX_REQ_HDR_LEN + 1U] = {0};
    (void)request_header_copy(req, "Cookie", cookie, sizeof(cookie),
                              &cookie_len, NULL);
    size_t offset = 0;
    bool found = copy_next_session_cookie(cookie, &offset, out, out_size);
    return found;
}

esp_err_t si_http_set_session_cookie(httpd_req_t *req, const char *token,
                                     char *cookie, size_t cookie_size)
{
    if (!req || !token || !token[0] || !cookie || cookie_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(
        cookie, cookie_size,
        "EA_SESSION=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u",
        token, (unsigned)SI_HTTP_SESSION_COOKIE_MAX_AGE_SECONDS);
    if (written < 0 || (size_t)written >= cookie_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    if (ret != ESP_OK) {
        return ret;
    }
    return httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t si_http_sync_browser_session_cookie_from_bearer(
    httpd_req_t *req, char *cookie, size_t cookie_size)
{
    if (!req || !cookie || cookie_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cookie[0] = '\0';

    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    if (!copy_bearer_token(req, token, sizeof(token))) {
        return ESP_ERR_NOT_FOUND;
    }

    si_auth_session_context_t session;
    if (!si_auth_token_get_context(token, false, &session) ||
        session.principal != SI_PRINCIPAL_BROWSER) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = si_http_set_session_cookie(req, token, cookie,
                                                cookie_size);
    si_secret_store_clear(token, sizeof(token));
    return ret;
}

void si_http_clear_session_cookie(httpd_req_t *req)
{
    if (!req) {
        return;
    }
    httpd_resp_set_hdr(
        req, "Set-Cookie",
        "EA_SESSION=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

bool si_http_check_auth(httpd_req_t *req)
{
    si_auth_session_context_t context;
    return si_http_get_session_context(req, &context);
}

bool si_http_get_session_context(httpd_req_t *req,
                                 si_auth_session_context_t *context)
{
    if (!req || !context) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->principal = SI_PRINCIPAL_ANONYMOUS;
    if (!si_auth_is_enabled()) {
        context->authenticated = true;
        context->principal = SI_PRINCIPAL_BROWSER;
        context->capabilities = SI_CAPABILITIES_WEB_DEFAULT;
        context->generation = SI_AUTH_DISABLED_SESSION_GENERATION;
        strlcpy(context->session_id, "auth-disabled",
                sizeof(context->session_id));
        return true;
    }
    char token[SI_AUTH_TOKEN_LEN + 1] = {0};
    size_t candidate_count = 0;
    if (copy_bearer_token(req, token, sizeof(token))) {
        candidate_count++;
        if (si_auth_token_get_context(token, true, context)) {
            if (!bootstrap_request_may_pass(req, context)) {
                memset(context, 0, sizeof(*context));
                context->principal = SI_PRINCIPAL_ANONYMOUS;
                return false;
            }
            auth_status_log_recovery(req);
            return true;
        }
    }

    size_t cookie_len = 0;
    const char *header_reason = NULL;
    char cookie[CONFIG_HTTPD_MAX_REQ_HDR_LEN + 1U] = {0};
    (void)request_header_copy(req, "Cookie", cookie, sizeof(cookie),
                              &cookie_len, &header_reason);
    size_t offset = 0;
    while (copy_next_session_cookie(cookie, &offset, token, sizeof(token))) {
        candidate_count++;
        if (si_auth_token_get_context(token, true, context)) {
            if (!bootstrap_request_may_pass(req, context)) {
                memset(context, 0, sizeof(*context));
                context->principal = SI_PRINCIPAL_ANONYMOUS;
                return false;
            }
            auth_status_log_recovery(req);
            return true;
        }
    }

    const char *reason = candidate_count > 0 ? "invalid-session" :
                                                   header_reason;
    if (!reason) {
        reason = "missing-token";
    }
    auth_status_log_failure(req, reason, cookie_len, candidate_count);
    return false;
}

esp_err_t si_http_require_auth(httpd_req_t *req)
{
    si_auth_session_context_t context;
    if (!si_http_get_session_context(req, &context)) {
        esp_err_t send_ret = httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;
    }
    if (!mcp_request_may_pass(req, &context)) {
        const char *reason = mcp_request_denial_reason(req, &context);
        esp_err_t send_ret = httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN,
            reason ? reason : "MCP request denied by tool policy");
        return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;
    }
    return ESP_OK;
}

static esp_err_t send_authorization_denial(
    httpd_req_t *req, si_authorization_decision_t decision)
{
    const char *reason = si_authorization_decision_name(decision);
    esp_err_t send_ret = ESP_FAIL;
    if (decision == SI_AUTHZ_DENY_UNAUTHENTICATED) {
        send_ret = httpd_resp_send_err(
            req, HTTPD_401_UNAUTHORIZED, reason);
    } else if (decision == SI_AUTHZ_DENY_KVM_PREEMPTED ||
               decision == SI_AUTHZ_DENY_LEASE ||
               decision == SI_AUTHZ_DENY_OBSERVE_ONLY) {
        send_ret = si_http_send_text_status(req, "409 Conflict", reason);
    } else if (decision == SI_AUTHZ_DENY_INVALID_REQUEST) {
        send_ret = httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, reason);
    } else {
        send_ret = httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, reason);
    }
    /* A successfully transmitted denial is still a failed authorization.
     * Callers use a non-OK return to stop the protected handler. */
    return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;
}

esp_err_t si_http_require_authorization(
    httpd_req_t *req, si_authorization_request_t *request,
    si_auth_session_context_t *context)
{
    si_auth_session_context_t local_context;
    si_auth_session_context_t *resolved = context ? context : &local_context;
    if (!request || !si_http_get_session_context(req, resolved)) {
        return send_authorization_denial(req,
                                         SI_AUTHZ_DENY_UNAUTHENTICATED);
    }
    if (!mcp_request_may_pass(req, resolved)) {
        const char *reason = mcp_request_denial_reason(req, resolved);
        esp_err_t send_ret = httpd_resp_send_err(
            req, HTTPD_403_FORBIDDEN,
            reason ? reason : "MCP request denied by tool policy");
        return send_ret == ESP_OK ? ESP_ERR_INVALID_STATE : send_ret;
    }
    request->authenticated = resolved->authenticated;
    request->principal = resolved->principal;
    request->capabilities = resolved->capabilities;
    si_authorization_decision_t decision =
        si_authorization_evaluate(request);
    return decision == SI_AUTHZ_ALLOW ? ESP_OK :
           send_authorization_denial(req, decision);
}

esp_err_t si_http_require_capability(
    httpd_req_t *req, si_capability_t required_capability,
    si_auth_session_context_t *context)
{
    si_authorization_request_t request = {
        .required_capability = required_capability,
        .policy_allowed = true,
        .mode = SI_AUTHZ_MODE_OBSERVE,
        .risk = SI_AUTHZ_RISK_LOW,
    };
    return si_http_require_authorization(req, &request, context);
}

static si_authorization_mode_t access_authorization_mode(
    si_agent_access_mode_t mode)
{
    if (mode == SI_AGENT_ACCESS_FULL) {
        return SI_AUTHZ_MODE_AUTONOMOUS;
    }
    if (mode == SI_AGENT_ACCESS_ASSISTED) {
        return SI_AUTHZ_MODE_SUPERVISED;
    }
    return SI_AUTHZ_MODE_OBSERVE;
}

esp_err_t si_http_require_action(
    httpd_req_t *req, si_capability_t required_capability,
    const char *tool_name, si_authorization_risk_t risk,
    si_auth_session_context_t *context)
{
    si_auth_session_context_t local_context;
    si_auth_session_context_t *resolved = context ? context : &local_context;
    esp_err_t auth_ret =
        si_http_require_capability(req, required_capability, resolved);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    bool is_mcp = resolved->principal == SI_PRINCIPAL_MCP;
    char policy_reason[96] = {0};
    bool policy_allowed =
        !is_mcp ||
        si_mcp_tool_policy_allows(tool_name, policy_reason,
                                  sizeof(policy_reason));
    si_agent_access_mode_t access_mode = si_agent_access_mode_get();
    if (is_mcp &&
        (access_mode == SI_AGENT_ACCESS_MANUAL ||
         (access_mode == SI_AGENT_ACCESS_ASSISTED &&
          risk > SI_AUTHZ_RISK_MEDIUM))) {
        policy_allowed = false;
    }
    si_control_lease_status_t lease;
    si_control_lease_get_status(&lease);
    si_authorization_request_t authorization = {
        .required_capability = required_capability,
        .policy_allowed = policy_allowed,
        .lease_required = is_mcp,
        .lease_held =
            !is_mcp ||
            si_control_lease_owner_session_matches(
                "mcp", resolved->session_id),
        .human_kvm_active = is_mcp && lease.kvm_active,
        .mode = is_mcp ? access_authorization_mode(access_mode) :
                         SI_AUTHZ_MODE_SUPERVISED,
        .risk = risk,
    };
    return si_http_require_authorization(req, &authorization, resolved);
}
