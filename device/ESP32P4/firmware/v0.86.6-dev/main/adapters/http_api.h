#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "auth_service.h"
#include "authorization.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#define SI_HTTP_SESSION_COOKIE_MAX_LEN 160

esp_err_t si_http_send_json(httpd_req_t *req, cJSON *root);
void si_http_set_security_headers(httpd_req_t *req);
esp_err_t si_http_send_text_status(httpd_req_t *req, const char *status,
                                   const char *message);

esp_err_t si_http_recv_body(httpd_req_t *req, char *buf, size_t buf_size);
esp_err_t si_http_recv_json(httpd_req_t *req, char *buf, size_t buf_size,
                            cJSON **out_root);

bool si_http_check_auth(httpd_req_t *req);
bool si_http_get_auth_token(httpd_req_t *req, char *out, size_t out_size);
bool si_http_get_session_context(httpd_req_t *req,
                                 si_auth_session_context_t *context);
esp_err_t si_http_set_session_cookie(httpd_req_t *req, const char *token,
                                     char *cookie, size_t cookie_size);
esp_err_t si_http_sync_browser_session_cookie_from_bearer(
    httpd_req_t *req, char *cookie, size_t cookie_size);
void si_http_clear_session_cookie(httpd_req_t *req);
esp_err_t si_http_require_auth(httpd_req_t *req);
esp_err_t si_http_require_capability(
    httpd_req_t *req, si_capability_t required_capability,
    si_auth_session_context_t *context);
esp_err_t si_http_require_authorization(
    httpd_req_t *req, si_authorization_request_t *request,
    si_auth_session_context_t *context);
esp_err_t si_http_require_action(
    httpd_req_t *req, si_capability_t required_capability,
    const char *tool_name, si_authorization_risk_t risk,
    si_auth_session_context_t *context);
