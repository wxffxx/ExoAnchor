// Transitional domain module: OTA settings. Kept in the web compilation unit until its service contract is independent.
static bool ota_url_is_valid(const char *url)
{
    if (!url || url[0] == '\0' || strlen(url) > OTA_URL_MAX_LEN) {
        return false;
    }
    return strncmp(url, "https://", 8) == 0;
}

static bool ota_channel_is_valid(const char *channel)
{
    if (!channel) {
        return false;
    }
    size_t len = strlen(channel);
    if (len < 1 || len > OTA_CHANNEL_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)channel[i];
        if (!isalnum(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

static void ota_settings_load(void)
{
    if (s_ota_settings_loaded) {
        return;
    }

    strlcpy(s_ota_manifest_url, OTA_MANIFEST_URL_DEFAULT, sizeof(s_ota_manifest_url));
    strlcpy(s_ota_channel, OTA_CHANNEL_DEFAULT, sizeof(s_ota_channel));
    s_ota_auto_check = false;

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t err = si_settings_store_open_read(&store, OTA_NAMESPACE);
    if (err == ESP_OK) {
        char url[sizeof(s_ota_manifest_url)] = {0};
        if (si_settings_store_get_string(
                &store, OTA_MANIFEST_URL_KEY, url, sizeof(url)) == ESP_OK &&
            ota_url_is_valid(url)) {
            strlcpy(s_ota_manifest_url, url, sizeof(s_ota_manifest_url));
        }

        char channel[sizeof(s_ota_channel)] = {0};
        if (si_settings_store_get_string(
                &store, OTA_CHANNEL_KEY, channel, sizeof(channel)) == ESP_OK &&
            ota_channel_is_valid(channel)) {
            strlcpy(s_ota_channel, channel, sizeof(s_ota_channel));
        }

        uint8_t auto_check = 0;
        if (si_settings_store_get_u8(
                &store, OTA_AUTO_CHECK_KEY, &auto_check) == ESP_OK) {
            s_ota_auto_check = auto_check != 0;
        }
        si_settings_store_close(&store);
    }

    s_ota_settings_loaded = true;
}

static void ota_settings_snapshot(ota_settings_t *settings)
{
    ota_settings_load();
    strlcpy(settings->manifest_url, s_ota_manifest_url, sizeof(settings->manifest_url));
    strlcpy(settings->channel, s_ota_channel, sizeof(settings->channel));
    settings->auto_check = s_ota_auto_check;
}

static esp_err_t ota_settings_save(const char *manifest_url, const char *channel, bool auto_check)
{
    if (!manifest_url) {
        manifest_url = "";
    }
    if (!channel || channel[0] == '\0') {
        channel = OTA_CHANNEL_DEFAULT;
    }
    if (manifest_url[0] != '\0' && !ota_url_is_valid(manifest_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_channel_is_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    ESP_RETURN_ON_ERROR(
        si_settings_store_open_write(&store, OTA_NAMESPACE),
        TAG, "open ota settings");
    esp_err_t ret = si_settings_store_set_string(
        &store, OTA_MANIFEST_URL_KEY, manifest_url);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(
            &store, OTA_CHANNEL_KEY, channel);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(
            &store, OTA_AUTO_CHECK_KEY, auto_check ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    ESP_RETURN_ON_ERROR(ret, TAG, "save ota settings");

    strlcpy(s_ota_manifest_url, manifest_url, sizeof(s_ota_manifest_url));
    strlcpy(s_ota_channel, channel, sizeof(s_ota_channel));
    s_ota_auto_check = auto_check;
    s_ota_settings_loaded = true;
    return ESP_OK;
}

static esp_err_t ota_require_auth(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_OTA, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    si_auth_status_t auth_status;
    si_auth_get_status(&auth_status);
    if (auth_status.enabled && auth_status.using_default) {
        return si_http_send_text_status(req, "403 Forbidden", "change default credentials before OTA");
    }
    return ESP_OK;
}

static void ota_set_last_message(const char *message)
{
    strlcpy(s_ota_last_message, message ? message : "", sizeof(s_ota_last_message));
    s_ota_last_check_sec = uptime_sec();
}

static const char *json_string_any4(const cJSON *obj, const char *first, const char *second,
                                    const char *third, const char *fourth)
{
    const char *value = json_string_any(obj, first, second, third);
    if (value) {
        return value;
    }
    const cJSON *item = fourth ? cJSON_GetObjectItemCaseSensitive(obj, fourth) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *json_number_any(const cJSON *obj, const char *first, const char *second)
{
    const cJSON *item = first ? cJSON_GetObjectItemCaseSensitive(obj, first) : NULL;
    if (cJSON_IsNumber(item)) {
        return item;
    }
    item = second ? cJSON_GetObjectItemCaseSensitive(obj, second) : NULL;
    return cJSON_IsNumber(item) ? item : NULL;
}
