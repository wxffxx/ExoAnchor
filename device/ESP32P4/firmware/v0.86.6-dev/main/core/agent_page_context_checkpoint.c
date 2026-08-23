#include "agent_page_context_checkpoint.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static esp_err_t clear_checkpoint_field(cJSON *root, const char *name,
                                        bool *changed_out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return ESP_OK;
    }
    if (cJSON_IsString(item) && item->valuestring &&
        item->valuestring[0] == '\0') {
        return ESP_OK;
    }
    cJSON *empty = cJSON_CreateString("");
    if (!empty) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_ReplaceItemInObjectCaseSensitive(root, name, empty)) {
        cJSON_Delete(empty);
        return ESP_FAIL;
    }
    *changed_out = true;
    return ESP_OK;
}

esp_err_t si_agent_page_context_strip_checkpoint(const char *json,
                                                 char **json_out,
                                                 bool *changed_out)
{
    if (!json || !json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;
    if (changed_out) {
        *changed_out = false;
    }

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    esp_err_t ret = clear_checkpoint_field(root, "page_context_json", &changed);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return ret;
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "page_context")) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "page_context");
        changed = true;
    }

    char *sanitized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!sanitized) {
        return ESP_ERR_NO_MEM;
    }
    *json_out = sanitized;
    if (changed_out) {
        *changed_out = changed;
    }
    return ESP_OK;
}
