#include "hid_json.h"

#include <stdint.h>
#include <string.h>

#include "hid_device.h"

static int json_int(const cJSON *object, const char *name, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static double json_double(const cJSON *object, const char *name, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_has_number(const cJSON *object, const char *name)
{
    return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(object, name));
}

void si_hid_json_add_action_result(cJSON *results, int index, esp_err_t err,
                                   const char *message)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddBoolToObject(item, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(item, "error",
                                message ? message : esp_err_to_name(err));
    }
    cJSON_AddItemToArray(results, item);
}

uint32_t si_hid_json_action_wait_ms(const cJSON *action)
{
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(action, "ms");
    if (!cJSON_IsNumber(ms)) {
        ms = cJSON_GetObjectItemCaseSensitive(action, "duration_ms");
    }
    if (!cJSON_IsNumber(ms)) {
        ms = cJSON_GetObjectItemCaseSensitive(action, "durationMs");
    }
    int value = cJSON_IsNumber(ms) ? ms->valueint : 0;
    if (value < 0) {
        value = 0;
    } else if (value > 5000) {
        value = 5000;
    }
    return (uint32_t)value;
}

static void parse_combo(const cJSON *message, si_hid_command_t *command)
{
    const cJSON *item = NULL;
    const cJSON *modifiers = cJSON_GetObjectItemCaseSensitive(message, "modifiers");
    cJSON_ArrayForEach(item, modifiers) {
        if (cJSON_IsString(item) &&
            command->modifier_count < SI_HID_COMBO_MODIFIER_MAX) {
            command->modifiers[command->modifier_count++] = item->valuestring;
        }
    }

    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(message, "keys");
    cJSON_ArrayForEach(item, keys) {
        if (cJSON_IsString(item) && command->key_count < SI_HID_COMBO_KEY_MAX) {
            command->keys[command->key_count++] = item->valuestring;
        }
    }
}

esp_err_t si_hid_json_execute(const cJSON *message)
{
    si_hid_command_t command = {
        .type = SI_HID_COMMAND_INVALID,
        .error = "invalid hid json",
    };
    if (!cJSON_IsObject(message)) {
        return si_hid_execute(&command);
    }

    const char *type = json_string(message, "type");
    if (!type) {
        command.error = "hid type missing";
        return si_hid_execute(&command);
    }

    if (strcmp(type, "keydown") == 0) {
        command.type = SI_HID_COMMAND_KEY_DOWN;
        command.code = json_string(message, "code");
    } else if (strcmp(type, "keyup") == 0) {
        command.type = SI_HID_COMMAND_KEY_UP;
        command.code = json_string(message, "code");
    } else if (strcmp(type, "mousemove") == 0) {
        const char *unit = json_string(message, "unit");
        if (unit && (strcmp(unit, "hid") == 0 || strcmp(unit, "counts") == 0)) {
            command.type = SI_HID_COMMAND_MOUSE_MOVE_COUNTS;
            command.x = json_int(message, "dx", json_int(message, "x", 0));
            command.y = json_int(message, "dy", json_int(message, "y", 0));
        } else {
            command.type = SI_HID_COMMAND_MOUSE_MOVE;
            command.dx = (float)json_double(message, "dx", json_double(message, "x", 0));
            command.dy = (float)json_double(message, "dy", json_double(message, "y", 0));
        }
    } else if (strcmp(type, "absmove") == 0) {
        command.type = SI_HID_COMMAND_ABS_MOVE;
        command.has_x = true;
        command.has_y = true;
        command.x = json_int(message, "x", SI_HID_ABS_MAX / 2);
        command.y = json_int(message, "y", SI_HID_ABS_MAX / 2);
    } else if (strcmp(type, "absmousedown") == 0) {
        command.type = SI_HID_COMMAND_ABS_MOUSE_DOWN;
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "absmouseup") == 0) {
        command.type = SI_HID_COMMAND_ABS_MOUSE_UP;
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "absclick") == 0) {
        command.type = SI_HID_COMMAND_ABS_CLICK;
        command.has_x = json_has_number(message, "x");
        command.has_y = json_has_number(message, "y");
        command.x = json_int(message, "x", 0);
        command.y = json_int(message, "y", 0);
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "mousedown") == 0) {
        command.type = SI_HID_COMMAND_MOUSE_DOWN;
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "mouseup") == 0) {
        command.type = SI_HID_COMMAND_MOUSE_UP;
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "click") == 0) {
        command.type = SI_HID_COMMAND_CLICK;
        command.button = (uint8_t)json_int(message, "button", 0);
    } else if (strcmp(type, "wheel") == 0) {
        command.type = SI_HID_COMMAND_WHEEL;
        command.wheel_y = (int8_t)json_int(message, "deltaY", 0);
        command.wheel_x = (int8_t)json_int(message, "deltaX", 0);
    } else if (strcmp(type, "combo") == 0) {
        command.type = SI_HID_COMMAND_COMBO;
        parse_combo(message, &command);
    } else if (strcmp(type, "releaseall") == 0) {
        command.type = SI_HID_COMMAND_RELEASE_ALL;
    } else {
        command.type = SI_HID_COMMAND_UNSUPPORTED;
        command.error = "unsupported hid command";
    }

    return si_hid_execute(&command);
}
