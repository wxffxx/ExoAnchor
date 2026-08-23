// Shared MS2109 power control plus PrototypeV2.4-only EEPROM validation.
#if SI_CFG_MS2109_POWER_ENABLED || SI_CFG_MS2109_TEST_ENABLED

#if SI_CFG_MS2109_TEST_ENABLED
#define SI_MS2109_HTTP_MAX_BODY 4096U
#define SI_MS2109_PROGRAM_CONFIRMATION "PROGRAM MS2109 EEPROM"

static bool s_ms2109_probe_valid;
static uint8_t s_ms2109_probe_mask;
#endif

static void add_ms2109_status_json(cJSON *root)
{
#if SI_CFG_MS2109_TEST_ENABLED
    si_ms2109_test_status_t status;
    si_ms2109_test_get_status(&status);
    cJSON_AddStringToObject(root, "schema", "exoanchor.ms2109-test.v1");
#else
    si_ms2109_power_status_t status;
    (void)si_ms2109_power_get_status(&status);
    cJSON_AddStringToObject(root, "schema", "exoanchor.ms2109-power.v1");
#endif
    cJSON_AddBoolToObject(root, "supported", status.enabled);
    cJSON_AddBoolToObject(root, "enabled", status.enabled);
    cJSON_AddBoolToObject(root, "initialized", status.initialized);
    cJSON_AddBoolToObject(root, "power_on", status.power_on);
#if SI_CFG_MS2109_TEST_ENABLED
    cJSON_AddBoolToObject(root, "write_protected", status.write_protected);
#endif
    cJSON_AddNumberToObject(root, "operation_count", status.operation_count);
    cJSON_AddStringToObject(root, "last_result",
                            esp_err_to_name(status.last_result));

    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(power, "on", status.power_on);
    cJSON_AddStringToObject(power, "state_kind", "commanded");
    cJSON_AddBoolToObject(power, "rail_feedback_available", false);
    cJSON_AddNumberToObject(power, "switch_3v3_gpio", status.switch_gpio);
    cJSON_AddNumberToObject(power, "switch_3v3_output_level",
                            status.switch_output_level);
    cJSON_AddNumberToObject(power, "enable_1v2_gpio",
                            status.core_enable_gpio);
    cJSON_AddNumberToObject(power, "enable_1v2_output_level",
                            status.core_enable_output_level);
    cJSON_AddStringToObject(power, "sequence",
                            "3v3-before-1v2 / 1v2-before-3v3-off");

#if SI_CFG_MS2109_TEST_ENABLED
    cJSON *eeprom = cJSON_AddObjectToObject(root, "eeprom");
    cJSON_AddStringToObject(eeprom, "type", "AT24C16");
    cJSON_AddNumberToObject(eeprom, "size_bytes",
                            SI_MS2109_AT24C16_SIZE);
    cJSON_AddNumberToObject(eeprom, "page_size_bytes",
                            SI_MS2109_AT24C16_PAGE_SIZE);
    cJSON_AddNumberToObject(eeprom, "wp_gpio", status.wp_gpio);
    cJSON_AddNumberToObject(eeprom, "scl_gpio", status.scl_gpio);
    cJSON_AddNumberToObject(eeprom, "sda_gpio", status.sda_gpio);
    cJSON_AddNumberToObject(eeprom, "scl_level", status.last_scl_level);
    cJSON_AddNumberToObject(eeprom, "sda_level", status.last_sda_level);
    cJSON_AddBoolToObject(eeprom, "write_protected",
                          status.write_protected);
    cJSON_AddBoolToObject(eeprom, "probe_valid", s_ms2109_probe_valid);
    cJSON_AddNumberToObject(eeprom, "address_mask", s_ms2109_probe_mask);
    cJSON_AddBoolToObject(eeprom, "all_blocks_detected",
                          s_ms2109_probe_valid &&
                          s_ms2109_probe_mask == 0xffU);
    cJSON_AddBoolToObject(eeprom, "read_supported", true);
    cJSON_AddBoolToObject(eeprom, "program_supported", true);
    cJSON_AddBoolToObject(eeprom, "readback_verification", true);
    cJSON_AddNumberToObject(eeprom, "last_programmed_bytes",
                            status.last_programmed_bytes);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08" PRIx32, status.last_crc32);
    cJSON_AddStringToObject(eeprom, "last_crc32", crc);
    cJSON_AddBoolToObject(eeprom, "last_verified", status.last_verified);
#endif
}

static esp_err_t ms2109_send_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "status allocation failed");
    }
    add_ms2109_status_json(root);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ms2109_send_operation_error(httpd_req_t *req,
                                              esp_err_t result,
                                              const char *operation)
{
    if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid MS2109 operation request");
    }
    if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_TIMEOUT) {
        return si_http_send_text_status(req, "409 Conflict",
                                        "MS2109 power interface is busy or unavailable");
    }
    if (result == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "MS2109 power interface is not enabled");
    }
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(result));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               esp_err_to_name(result));
}

static esp_err_t ms2109_status_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OBSERVE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    return ms2109_send_status(req);
}

static esp_err_t ms2109_power_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_POWER, "ms2109_power",
        SI_AUTHZ_RISK_HIGH, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t automation_video = {0};
    si_video_control_get_status(&automation_video);
    if (automation_video.agent_active) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop Agent video observation before changing MS2109 power");
    }

    char body[256] = {0};
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(req, body, sizeof(body), &root);
    if (ret != ESP_OK) {
        return ret;
    }
    const char *requested = json_string_any(root, "action", "state", NULL);
    char action[16] = {0};
    if (requested) {
        snprintf(action, sizeof(action), "%s", requested);
    }
    cJSON *duration_item =
        cJSON_GetObjectItemCaseSensitive(root, "off_time_ms");
    uint32_t off_time_ms = cJSON_IsNumber(duration_item) ?
                           (uint32_t)duration_item->valueint : 250U;
    cJSON_Delete(root);

    if (strcasecmp(action, "on") == 0) {
#if SI_CFG_MS2109_TEST_ENABLED
        ret = si_ms2109_test_set_power(true);
#else
        ret = si_ms2109_power_set(true);
#endif
    } else if (strcasecmp(action, "off") == 0) {
#if SI_CFG_MS2109_TEST_ENABLED
        ret = si_ms2109_test_set_power(false);
#else
        ret = si_ms2109_power_set(false);
#endif
    } else if (strcasecmp(action, "cycle") == 0 ||
               strcasecmp(action, "restart") == 0) {
#if SI_CFG_MS2109_TEST_ENABLED
        ret = si_ms2109_test_cycle_power(off_time_ms);
#else
        ret = si_ms2109_power_cycle(off_time_ms);
#endif
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "action must be on, off, or cycle");
    }
    if (ret != ESP_OK) {
        return ms2109_send_operation_error(req, ret, "MS2109 power");
    }
    si_web_log("INFO", "MS2109 power control updated");
    return ms2109_send_status(req);
}

#if SI_CFG_MS2109_TEST_ENABLED
static esp_err_t ms2109_eeprom_probe_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_SETTINGS, "ms2109_eeprom_probe",
        SI_AUTHZ_RISK_MEDIUM, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    uint8_t mask = 0;
    esp_err_t ret = si_ms2109_test_probe_eeprom(&mask);
    s_ms2109_probe_valid = true;
    s_ms2109_probe_mask = mask;
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ms2109_send_operation_error(req, ret, "MS2109 EEPROM probe");
    }
    return ms2109_send_status(req);
}

static esp_err_t ms2109_eeprom_read_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_SETTINGS, "ms2109_eeprom_read",
        SI_AUTHZ_RISK_MEDIUM, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    uint8_t *image = agent_alloc(SI_MS2109_AT24C16_SIZE);
    const size_t encoded_capacity =
        4U * ((SI_MS2109_AT24C16_SIZE + 2U) / 3U) + 1U;
    unsigned char *encoded = agent_alloc(encoded_capacity);
    if (!image || !encoded) {
        free(image);
        free(encoded);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "EEPROM buffer allocation failed");
    }

    esp_err_t ret = si_ms2109_test_read_eeprom(
        0, image, SI_MS2109_AT24C16_SIZE);
    if (ret != ESP_OK) {
        free(image);
        free(encoded);
        return ms2109_send_operation_error(req, ret, "MS2109 EEPROM read");
    }
    size_t encoded_size = 0;
    int base64_ret = mbedtls_base64_encode(
        encoded, encoded_capacity, &encoded_size, image,
        SI_MS2109_AT24C16_SIZE);
    uint32_t crc32 = esp_rom_crc32_le(0, image, SI_MS2109_AT24C16_SIZE);
    free(image);
    if (base64_ret != 0 || encoded_size >= encoded_capacity) {
        free(encoded);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "EEPROM base64 encoding failed");
    }
    encoded[encoded_size] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(encoded);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "EEPROM response allocation failed");
    }
    cJSON_AddStringToObject(root, "type", "AT24C16");
    cJSON_AddNumberToObject(root, "size_bytes", SI_MS2109_AT24C16_SIZE);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08" PRIx32, crc32);
    cJSON_AddStringToObject(root, "crc32", crc);
    cJSON_AddStringToObject(root, "encoding", "base64");
    cJSON_AddStringToObject(root, "image_b64", (char *)encoded);
    free(encoded);
    ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ms2109_eeprom_program_handler(httpd_req_t *req)
{
    esp_err_t auth_ret = si_http_require_action(
        req, SI_CAPABILITY_SETTINGS, "ms2109_eeprom_program",
        SI_AUTHZ_RISK_HIGH, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_video_control_status_t automation_video = {0};
    si_video_control_get_status(&automation_video);
    if (automation_video.agent_active) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "stop Agent video observation before programming MS2109 EEPROM");
    }

    char *body = agent_alloc(SI_MS2109_HTTP_MAX_BODY);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "request allocation failed");
    }
    cJSON *root = NULL;
    esp_err_t ret = si_http_recv_json(
        req, body, SI_MS2109_HTTP_MAX_BODY, &root);
    if (ret != ESP_OK) {
        free(body);
        return ret;
    }
    const char *confirmation =
        json_string_any(root, "confirm", "confirmation", NULL);
    const char *encoded =
        json_string_any(root, "image_b64", "image", "data");
    if (!confirmation ||
        strcmp(confirmation, SI_MS2109_PROGRAM_CONFIRMATION) != 0) {
        cJSON_Delete(root);
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "explicit EEPROM programming confirmation required");
    }
    if (!encoded) {
        cJSON_Delete(root);
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "image_b64 is required");
    }

    uint8_t *image = agent_alloc(SI_MS2109_AT24C16_SIZE);
    if (!image) {
        cJSON_Delete(root);
        free(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "EEPROM image allocation failed");
    }
    size_t decoded_size = 0;
    int base64_ret = mbedtls_base64_decode(
        image, SI_MS2109_AT24C16_SIZE, &decoded_size,
        (const unsigned char *)encoded, strlen(encoded));
    cJSON_Delete(root);
    free(body);
    if (base64_ret != 0 || decoded_size != SI_MS2109_AT24C16_SIZE) {
        free(image);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "EEPROM image must decode to exactly 2048 bytes");
    }

    uint32_t crc32 = 0;
    bool verified = false;
    ret = si_ms2109_test_program_eeprom(
        image, decoded_size, &crc32, &verified);
    free(image);
    if (ret != ESP_OK) {
        return ms2109_send_operation_error(req, ret,
                                           "MS2109 EEPROM program");
    }

    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "EEPROM response allocation failed");
    }
    cJSON_AddBoolToObject(response, "programmed", true);
    cJSON_AddBoolToObject(response, "verified", verified);
    cJSON_AddNumberToObject(response, "size_bytes", decoded_size);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08" PRIx32, crc32);
    cJSON_AddStringToObject(response, "crc32", crc);
    cJSON *status = cJSON_AddObjectToObject(response, "status");
    add_ms2109_status_json(status);
    si_web_log("INFO", "MS2109 EEPROM programmed and verified");
    ret = si_http_send_json(req, response);
    cJSON_Delete(response);
    return ret;
}

#endif
#endif
