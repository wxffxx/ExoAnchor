#include "http_api.h"

#include <stdlib.h>
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

bool si_http_check_auth(httpd_req_t *req)
{
    if (!si_auth_is_enabled()) {
        return true;
    }

    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth, sizeof(auth)) == ESP_OK) {
        const char *prefix = "Bearer ";
        if (strncmp(auth, prefix, strlen(prefix)) == 0 &&
            si_auth_token_matches(auth + strlen(prefix))) {
            return true;
        }
    }

    char query[192] = {0};
    char token[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "auth", token, sizeof(token)) == ESP_OK &&
        si_auth_token_matches(token)) {
        return true;
    }
    return false;
}

esp_err_t si_http_require_auth(httpd_req_t *req)
{
    if (!si_http_check_auth(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    return ESP_OK;
}
