#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "hid_device.h"
#include "hid_json.h"

static si_hid_command_type_t s_type;
static char s_error[64];
static char s_code[32];

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test error";
}

esp_err_t si_hid_execute_owned(const si_hid_command_t *command,
                               const si_hid_owner_token_t *owner,
                               si_hid_authority_guard_fn guard,
                               void *guard_context)
{
    assert(owner != NULL);
    if (guard && !guard(guard_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_type = command ? command->type : SI_HID_COMMAND_INVALID;
    snprintf(s_error, sizeof(s_error), "%s",
             command && command->error ? command->error : "");
    snprintf(s_code, sizeof(s_code), "%s",
             command && command->code ? command->code : "");
    return ESP_OK;
}

static void execute_json(const char *json)
{
    cJSON *message = cJSON_Parse(json);
    assert(message);
    const si_hid_owner_token_t owner = {
        .claim = {
            .kind = SI_HID_OWNER_CONTROL_LEASE,
            .session_id = "test-owner",
            .authority_epoch = 1U,
        },
        .generation = 1U,
    };
    assert(si_hid_json_execute_owned(message, &owner, NULL, NULL) == ESP_OK);
    cJSON_Delete(message);
}

int main(void)
{
    execute_json("{\"type\":\"keydown\",\"code\":\"KeyA\"}");
    assert(s_type == SI_HID_COMMAND_KEY_DOWN);
    assert(strcmp(s_code, "KeyA") == 0);
    assert(s_error[0] == '\0');

    execute_json("{\"type\":\"mousemove\",\"dx\":0.5,\"dy\":-0.25}");
    assert(s_type == SI_HID_COMMAND_MOUSE_MOVE);
    assert(s_error[0] == '\0');

    execute_json("{\"type\":\"releaseall\"}");
    assert(s_type == SI_HID_COMMAND_RELEASE_ALL);
    assert(s_error[0] == '\0');

    execute_json("{\"type\":\"future-command\"}");
    assert(s_type == SI_HID_COMMAND_UNSUPPORTED);
    assert(strcmp(s_error, "unsupported hid command") == 0);

    execute_json("[]");
    assert(s_type == SI_HID_COMMAND_INVALID);
    assert(strcmp(s_error, "invalid hid json") == 0);

    puts("hid json tests: PASS");
    return 0;
}
