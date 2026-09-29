// Destructive system maintenance runs only after Agent/MCP resources quiesce.
static void system_reset_everything_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(650));
    si_auth_revoke_all_sessions();
    ESP_LOGW(TAG, "Reset everything requested: erasing default NVS partition");
    esp_err_t ret = nvs_flash_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS deinit before reset failed: %s", esp_err_to_name(ret));
    }
    ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed during reset everything: %s", esp_err_to_name(ret));
    }
    esp_restart();
}

static esp_err_t settings_system_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_SETTINGS, NULL);
    if (auth_ret != ESP_OK) return auth_ret;
    if (req->method == HTTP_GET) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "reset_everything_available", true);
        cJSON_AddStringToObject(resp, "confirm_phrase", "RESET EVERYTHING");
        esp_err_t ret = si_http_send_json(req, resp);
        cJSON_Delete(resp);
        return ret;
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                                   "method not allowed");
    }
    char buf[192] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (ret != ESP_OK) return ret;
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(root, "reset_everything");
    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    bool confirmed = cJSON_IsTrue(reset) && cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "RESET EVERYTHING") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "reset_everything and confirm phrase required");
    }
    si_resource_operation_token_t maintenance = {0};
    if (!automation_begin_maintenance(
            "reset-everything", "正在恢复出厂设置", false, &maintenance)) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "could not stop active Agent/MCP operations before reset");
    }
    si_web_log("WARN", "Reset everything requested");
    if (xTaskCreate(system_reset_everything_task, "si_reset_all", 3072, NULL,
                    tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
        (void)resource_operation_finish(&maintenance);
        return ESP_ERR_NO_MEM;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "erasing settings and rebooting");
    cJSON_AddBoolToObject(resp, "rebooting", true);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}
