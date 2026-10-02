#include "http_api.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "auth_service.h"

esp_err_t si_http_send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "json alloc failed");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

esp_err_t si_http_send_text_status(httpd_req_t *req, const char *status,
                                   const char *message)
{
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
        esp_err_t sent = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                            "request body too large");
        return sent == ESP_OK ? ESP_ERR_INVALID_SIZE : sent;
    }
    if (body_ret != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        esp_err_t sent = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                            "invalid json");
        return sent == ESP_OK ? ESP_ERR_INVALID_ARG : sent;
    }
    *out_root = root;
    return ESP_OK;
}

bool si_http_get_auth_token(httpd_req_t *req, char *out, size_t out_size)
{
    if (!req || !out || out_size < SI_AUTH_TOKEN_LEN + 1) return false;
    out[0] = '\0';
    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK &&
        strncmp(auth, "Bearer ", 7) == 0 && strlen(auth + 7) == SI_AUTH_TOKEN_LEN) {
        strlcpy(out, auth + 7, out_size);
        return true;
    }
    char cookie[1024] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char *part = cookie;
    while (part && *part) {
        while (*part == ' ' || *part == ';') ++part;
        char *next = strchr(part, ';');
        if (next) *next++ = '\0';
        if (strncmp(part, "ea_session=", 11) == 0 && strlen(part + 11) == SI_AUTH_TOKEN_LEN) {
            strlcpy(out, part + 11, out_size);
            return true;
        }
        part = next;
    }
    return false;
}

bool si_http_check_auth(httpd_req_t *req)
{
    char token[SI_AUTH_TOKEN_LEN + 1];
    return si_http_get_auth_token(req, token, sizeof(token)) && si_auth_token_matches(token);
}

bool si_http_check_recent_auth(httpd_req_t *req)
{
    char token[SI_AUTH_TOKEN_LEN + 1];
    return si_http_get_auth_token(req, token, sizeof(token)) && si_auth_token_is_recent(token);
}

esp_err_t si_http_set_session_cookie(httpd_req_t *req, const char *token, char *cookie, size_t cookie_size)
{
    if (!token || strlen(token) != SI_AUTH_TOKEN_LEN || cookie_size < SI_HTTP_SESSION_COOKIE_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    snprintf(cookie, cookie_size, "ea_session=%s; Path=/; HttpOnly; Secure; SameSite=Strict", token);
    return httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

void si_http_clear_session_cookie(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Set-Cookie", "ea_session=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0");
}

esp_err_t si_http_require_auth(httpd_req_t *req)
{
    if (!si_http_check_auth(req)) {
        esp_err_t sent = httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                                            "unauthorized");
        return sent == ESP_OK ? ESP_ERR_INVALID_STATE : sent;
    }
    return ESP_OK;
}
