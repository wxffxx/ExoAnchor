#include "network_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "net_manager.h"

#define NETWORK_HTTP_BODY_MAX 1024U
#define NETWORK_OPERATION_DELAY_MS 400U

typedef enum {
    NETWORK_HTTP_APPLY = 1,
    NETWORK_HTTP_ROLLBACK,
    NETWORK_HTTP_RESET,
} network_http_operation_t;

static void add_network_config(cJSON *root,
                               const si_network_config_t *config)
{
    cJSON_AddNumberToObject(root, "schema_version", config->schema_version);
    cJSON_AddNumberToObject(root, "generation", config->generation);
    cJSON_AddStringToObject(root, "mode",
                            si_network_mode_name(config->mode));
    cJSON_AddBoolToObject(root, "autoip_fallback",
                          config->autoip_fallback);
    cJSON_AddStringToObject(root, "hostname", config->hostname);
    cJSON_AddStringToObject(root, "address", config->address);
    cJSON_AddStringToObject(root, "netmask", config->netmask);
    cJSON_AddStringToObject(root, "gateway", config->gateway);
    cJSON_AddStringToObject(root, "dns_primary", config->dns_primary);
    cJSON_AddStringToObject(root, "dns_secondary", config->dns_secondary);
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool copy_required(char *destination, size_t destination_size,
                          const char *value)
{
    if (!destination || destination_size == 0 || !value ||
        value[0] == '\0' || strlen(value) >= destination_size) {
        return false;
    }
    strlcpy(destination, value, destination_size);
    return true;
}

static bool copy_optional(char *destination, size_t destination_size,
                          const char *value)
{
    if (!destination || destination_size == 0 || !value ||
        strlen(value) >= destination_size) {
        return false;
    }
    if (strcmp(value, "-") == 0) {
        destination[0] = '\0';
    } else {
        strlcpy(destination, value, destination_size);
    }
    return true;
}

static void network_operation_task(void *argument)
{
    network_http_operation_t operation =
        (network_http_operation_t)(uintptr_t)argument;
    vTaskDelay(pdMS_TO_TICKS(NETWORK_OPERATION_DELAY_MS));
    if (operation == NETWORK_HTTP_APPLY) {
        (void)si_net_apply_staged();
    } else if (operation == NETWORK_HTTP_ROLLBACK) {
        (void)si_net_rollback_pending();
    } else if (operation == NETWORK_HTTP_RESET) {
        (void)si_net_reset_config();
    }
    vTaskDelete(NULL);
}

static esp_err_t schedule_network_operation(
    httpd_req_t *req, network_http_operation_t operation,
    const char *action)
{
    BaseType_t created =
        xTaskCreate(network_operation_task, "si_net_http", 3072,
                    (void *)(uintptr_t)operation, tskIDLE_PRIORITY + 2,
                    NULL);
    if (created != pdPASS) {
        return si_http_send_text_status(
            req, "503 Service Unavailable",
            "network operation worker unavailable");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddBoolToObject(root, "scheduled", true);
    cJSON_AddStringToObject(
        root, "message",
        "network operation scheduled; reconnect if the device address changes");
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t send_network_operation_result(
    httpd_req_t *req, const char *action, esp_err_t result)
{
    if (result == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid network configuration");
    }
    if (result == ESP_ERR_INVALID_STATE ||
        result == ESP_ERR_NOT_FOUND) {
        return si_http_send_text_status(req, "409 Conflict",
                                        esp_err_to_name(result));
    }
    if (result != ESP_OK) {
        return si_http_send_text_status(req, "503 Service Unavailable",
                                        esp_err_to_name(result));
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddBoolToObject(root, "scheduled", false);
    char message[80];
    snprintf(message, sizeof(message), "network %s accepted", action);
    cJSON_AddStringToObject(root, "message", message);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t si_network_status_http_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_net_status_t status;
    si_net_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "configured", status.configured);
    cJSON_AddBoolToObject(root, "initialized", status.initialized);
    cJSON_AddBoolToObject(root, "link_up", status.link_up);
    cJSON_AddBoolToObject(root, "connected", status.connected);
    cJSON_AddNumberToObject(root, "speed_mbps", status.speed_mbps);
    cJSON_AddBoolToObject(root, "full_duplex", status.full_duplex);
    cJSON_AddStringToObject(root, "interface", status.interface);
    cJSON_AddStringToObject(root, "device_id", status.device_id);
    cJSON_AddStringToObject(root, "hostname", status.hostname);
    cJSON_AddStringToObject(root, "mode", status.mode);
    cJSON_AddStringToObject(root, "address_source",
                            status.address_source);
    cJSON_AddStringToObject(root, "config_state", status.config_state);
    cJSON_AddNumberToObject(root, "config_generation",
                            status.config_generation);
    cJSON_AddBoolToObject(root, "pending_confirmation",
                          status.pending_confirmation);
    cJSON_AddNumberToObject(root, "confirm_remaining_seconds",
                            status.confirm_remaining_seconds);
    cJSON_AddBoolToObject(root, "recovered_pending",
                          status.recovered_pending);
    cJSON_AddBoolToObject(root, "recovery_active",
                          status.recovery_active);
    cJSON_AddStringToObject(root, "ipv4", status.ip);
    cJSON_AddStringToObject(root, "netmask", status.netmask);
    cJSON_AddStringToObject(root, "gateway", status.gateway);
    cJSON_AddStringToObject(root, "dns_primary", status.dns_primary);
    cJSON_AddStringToObject(root, "dns_secondary", status.dns_secondary);
    cJSON_AddStringToObject(root, "mac", status.mac);
    cJSON_AddStringToObject(root, "last_error", status.last_error);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t si_network_config_http_handler(httpd_req_t *req)
{
    si_capability_t required =
        req->method == HTTP_GET ? SI_CAPABILITY_OBSERVE :
        req->method == HTTP_POST ? SI_CAPABILITY_SETTINGS :
                                   SI_CAPABILITY_NONE;
    if (required == SI_CAPABILITY_NONE) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }
    esp_err_t auth_ret =
        si_http_require_capability(req, required, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (req->method == HTTP_POST) {
        if (req->content_len <= 0 ||
            req->content_len >= (int)NETWORK_HTTP_BODY_MAX) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "json body required");
        }
        char *body = calloc(1, NETWORK_HTTP_BODY_MAX);
        if (!body) {
            return httpd_resp_send_err(
                req, HTTPD_500_INTERNAL_SERVER_ERROR,
                "request alloc failed");
        }
        cJSON *root = NULL;
        esp_err_t ret =
            si_http_recv_json(req, body, NETWORK_HTTP_BODY_MAX, &root);
        free(body);
        if (ret != ESP_OK) {
            return ret;
        }
        const char *action = json_string(root, "action");
        if (!action) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "network action is required");
        }
        if (strcmp(action, "apply") == 0 ||
            strcmp(action, "rollback") == 0 ||
            strcmp(action, "reset") == 0) {
            if (strcmp(action, "reset") == 0) {
                const cJSON *confirm =
                    cJSON_GetObjectItemCaseSensitive(root, "confirm");
                if (!cJSON_IsTrue(confirm)) {
                    cJSON_Delete(root);
                    return httpd_resp_send_err(
                        req, HTTPD_400_BAD_REQUEST,
                        "confirm=true required for network reset");
                }
            }
            network_http_operation_t operation =
                strcmp(action, "apply") == 0 ? NETWORK_HTTP_APPLY :
                strcmp(action, "rollback") == 0 ? NETWORK_HTTP_ROLLBACK :
                                                  NETWORK_HTTP_RESET;
            const char *scheduled_action =
                operation == NETWORK_HTTP_APPLY ? "apply" :
                operation == NETWORK_HTTP_ROLLBACK ? "rollback" :
                                                     "reset";
            cJSON_Delete(root);
            return schedule_network_operation(req, operation,
                                              scheduled_action);
        }
        if (strcmp(action, "commit") == 0) {
            cJSON_Delete(root);
            return send_network_operation_result(
                req, "commit", si_net_commit_pending());
        }

        si_network_config_t config;
        const char *result_action = "unknown";
        ret = si_net_get_staged_config(&config);
        if (ret == ESP_ERR_NOT_FOUND) {
            ret = si_net_get_active_config(&config);
        }
        if (ret != ESP_OK) {
            cJSON_Delete(root);
            return si_http_send_text_status(
                req, "503 Service Unavailable",
                "network configuration unavailable");
        }
        if (strcmp(action, "dhcp") == 0) {
            result_action = "dhcp";
            const char *hostname = json_string(root, "hostname");
            if (hostname &&
                !copy_required(config.hostname,
                               sizeof(config.hostname), hostname)) {
                ret = ESP_ERR_INVALID_ARG;
            } else {
                config.mode = SI_NETWORK_MODE_DHCP;
                config.autoip_fallback = true;
                config.address[0] = '\0';
                config.netmask[0] = '\0';
                config.gateway[0] = '\0';
                config.dns_primary[0] = '\0';
                config.dns_secondary[0] = '\0';
                ret = si_net_stage_config(&config);
            }
        } else if (strcmp(action, "hostname") == 0) {
            result_action = "hostname";
            const char *hostname = json_string(root, "hostname");
            ret = copy_required(config.hostname,
                                sizeof(config.hostname), hostname) ?
                  si_net_stage_config(&config) : ESP_ERR_INVALID_ARG;
        } else if (strcmp(action, "static") == 0) {
            result_action = "static";
            const char *address = json_string(root, "address");
            const char *netmask = json_string(root, "netmask");
            const char *gateway = json_string(root, "gateway");
            const char *dns_primary = json_string(root, "dns_primary");
            const char *dns_secondary =
                json_string(root, "dns_secondary");
            bool valid =
                copy_required(config.address, sizeof(config.address),
                              address) &&
                copy_required(config.netmask, sizeof(config.netmask),
                              netmask) &&
                copy_optional(config.gateway, sizeof(config.gateway),
                              gateway ? gateway : "-") &&
                copy_optional(config.dns_primary,
                              sizeof(config.dns_primary),
                              dns_primary ? dns_primary : "-") &&
                copy_optional(config.dns_secondary,
                              sizeof(config.dns_secondary),
                              dns_secondary ? dns_secondary : "-");
            if (valid) {
                config.mode = SI_NETWORK_MODE_STATIC;
                config.autoip_fallback = false;
                ret = si_net_stage_config(&config);
            } else {
                ret = ESP_ERR_INVALID_ARG;
            }
        } else {
            ret = ESP_ERR_INVALID_ARG;
        }
        cJSON_Delete(root);
        return send_network_operation_result(req, result_action, ret);
    }

    si_network_config_t active;
    esp_err_t ret = si_net_get_active_config(&active);
    if (ret != ESP_OK) {
        return si_http_send_text_status(
            req, "503 Service Unavailable",
            "network configuration unavailable");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *active_json = cJSON_AddObjectToObject(root, "active");
    if (!active_json) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    add_network_config(active_json, &active);
    si_network_config_t staged;
    ret = si_net_get_staged_config(&staged);
    cJSON_AddBoolToObject(root, "have_staged", ret == ESP_OK);
    if (ret == ESP_OK) {
        cJSON *staged_json = cJSON_AddObjectToObject(root, "staged");
        add_network_config(staged_json, &staged);
    }
    ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}
