#include "hid_device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "si-hid";

#ifdef CONFIG_SI_HID_USB_ENABLE
#define SI_HID_CFG_ENABLE 1
#else
#define SI_HID_CFG_ENABLE 0
#endif

#ifndef CONFIG_SI_HID_USB_DM_GPIO
#define CONFIG_SI_HID_USB_DM_GPIO 26
#endif
#ifndef CONFIG_SI_HID_USB_DP_GPIO
#define CONFIG_SI_HID_USB_DP_GPIO 27
#endif

#define HID_ITF_KEYBOARD 0
#define HID_ITF_MOUSE 1
#define HID_ITF_ABS_POINTER 2
#define HID_EP_KEYBOARD 0x81
#define HID_EP_MOUSE 0x82
#define HID_EP_ABS_POINTER 0x83
#define HID_EP_SIZE 8
#define HID_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN)
#define HID_ABS_MAX 32767

typedef struct {
    const char *code;
    uint8_t hid;
} key_mapping_t;

typedef struct {
    const char *code;
    uint8_t mask;
} mod_mapping_t;

static const key_mapping_t KEY_MAP[] = {
    {"KeyA", 0x04}, {"KeyB", 0x05}, {"KeyC", 0x06}, {"KeyD", 0x07},
    {"KeyE", 0x08}, {"KeyF", 0x09}, {"KeyG", 0x0A}, {"KeyH", 0x0B},
    {"KeyI", 0x0C}, {"KeyJ", 0x0D}, {"KeyK", 0x0E}, {"KeyL", 0x0F},
    {"KeyM", 0x10}, {"KeyN", 0x11}, {"KeyO", 0x12}, {"KeyP", 0x13},
    {"KeyQ", 0x14}, {"KeyR", 0x15}, {"KeyS", 0x16}, {"KeyT", 0x17},
    {"KeyU", 0x18}, {"KeyV", 0x19}, {"KeyW", 0x1A}, {"KeyX", 0x1B},
    {"KeyY", 0x1C}, {"KeyZ", 0x1D},
    {"Digit1", 0x1E}, {"Digit2", 0x1F}, {"Digit3", 0x20}, {"Digit4", 0x21},
    {"Digit5", 0x22}, {"Digit6", 0x23}, {"Digit7", 0x24}, {"Digit8", 0x25},
    {"Digit9", 0x26}, {"Digit0", 0x27},
    {"Enter", 0x28}, {"Escape", 0x29}, {"Backspace", 0x2A}, {"Tab", 0x2B},
    {"Space", 0x2C}, {"Minus", 0x2D}, {"Equal", 0x2E}, {"BracketLeft", 0x2F},
    {"BracketRight", 0x30}, {"Backslash", 0x31}, {"Semicolon", 0x33},
    {"Quote", 0x34}, {"Backquote", 0x35}, {"Comma", 0x36}, {"Period", 0x37},
    {"Slash", 0x38}, {"CapsLock", 0x39},
    {"F1", 0x3A}, {"F2", 0x3B}, {"F3", 0x3C}, {"F4", 0x3D},
    {"F5", 0x3E}, {"F6", 0x3F}, {"F7", 0x40}, {"F8", 0x41},
    {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45},
    {"PrintScreen", 0x46}, {"ScrollLock", 0x47}, {"Pause", 0x48},
    {"Insert", 0x49}, {"Home", 0x4A}, {"PageUp", 0x4B},
    {"Delete", 0x4C}, {"End", 0x4D}, {"PageDown", 0x4E},
    {"ArrowRight", 0x4F}, {"ArrowLeft", 0x50}, {"ArrowDown", 0x51},
    {"ArrowUp", 0x52}, {"NumLock", 0x53},
    {"NumpadDivide", 0x54}, {"NumpadMultiply", 0x55}, {"NumpadSubtract", 0x56},
    {"NumpadAdd", 0x57}, {"NumpadEnter", 0x58},
    {"Numpad1", 0x59}, {"Numpad2", 0x5A}, {"Numpad3", 0x5B},
    {"Numpad4", 0x5C}, {"Numpad5", 0x5D}, {"Numpad6", 0x5E},
    {"Numpad7", 0x5F}, {"Numpad8", 0x60}, {"Numpad9", 0x61},
    {"Numpad0", 0x62}, {"NumpadDecimal", 0x63},
    {"IntlBackslash", 0x64}, {"ContextMenu", 0x65}, {"Power", 0x66},
};

static const mod_mapping_t MOD_MAP[] = {
    {"ControlLeft", 0x01}, {"ShiftLeft", 0x02}, {"AltLeft", 0x04}, {"MetaLeft", 0x08},
    {"ControlRight", 0x10}, {"ShiftRight", 0x20}, {"AltRight", 0x40}, {"MetaRight", 0x80},
};

static const uint8_t HID_KEYBOARD_REPORT_DESCRIPTOR[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

static const uint8_t HID_MOUSE_REPORT_DESCRIPTOR[] = {
    TUD_HID_REPORT_DESC_MOUSE(),
};

static const uint8_t HID_ABS_POINTER_REPORT_DESCRIPTOR[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Constant, Variable, Absolute)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x00, 0x00,  //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

static const char *HID_STRING_DESCRIPTOR[] = {
    (char[]){0x09, 0x04},
    "ExoAnchor",
    "ESP32-P4 GPIO26/27 HID",
    "P4-FS-HID",
    "Keyboard",
    "Mouse",
    "Absolute Pointer",
};

static const uint8_t HID_CONFIGURATION_DESCRIPTOR[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, HID_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_KEYBOARD, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(HID_KEYBOARD_REPORT_DESCRIPTOR), HID_EP_KEYBOARD, HID_EP_SIZE, 10),
    TUD_HID_DESCRIPTOR(HID_ITF_MOUSE, 5, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(HID_MOUSE_REPORT_DESCRIPTOR), HID_EP_MOUSE, HID_EP_SIZE, 10),
    TUD_HID_DESCRIPTOR(HID_ITF_ABS_POINTER, 6, HID_ITF_PROTOCOL_NONE,
                       sizeof(HID_ABS_POINTER_REPORT_DESCRIPTOR), HID_EP_ABS_POINTER, HID_EP_SIZE, 5),
};

static bool s_installed;
static bool s_mounted;
static uint8_t s_modifiers;
static uint8_t s_keys[6];
static uint8_t s_key_count;
static uint8_t s_mouse_buttons;
static uint8_t s_abs_buttons;
static uint16_t s_abs_x = HID_ABS_MAX / 2;
static uint16_t s_abs_y = HID_ABS_MAX / 2;
static uint32_t s_tx_messages;
static uint32_t s_failed_messages;
static char s_last_error[96];

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case HID_ITF_KEYBOARD:
        return HID_KEYBOARD_REPORT_DESCRIPTOR;
    case HID_ITF_MOUSE:
        return HID_MOUSE_REPORT_DESCRIPTOR;
    case HID_ITF_ABS_POINTER:
        return HID_ABS_POINTER_REPORT_DESCRIPTOR;
    default:
        return HID_MOUSE_REPORT_DESCRIPTOR;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static void set_last_error(const char *message, esp_err_t err)
{
    if (err == ESP_OK) {
        s_last_error[0] = '\0';
        return;
    }
    snprintf(s_last_error, sizeof(s_last_error), "%s: %s",
             message ? message : "hid error", esp_err_to_name(err));
}

static uint8_t js_code_to_hid(const char *code)
{
    if (!code) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(KEY_MAP) / sizeof(KEY_MAP[0]); i++) {
        if (strcmp(KEY_MAP[i].code, code) == 0) {
            return KEY_MAP[i].hid;
        }
    }
    return 0;
}

static uint8_t js_code_to_mod(const char *code)
{
    if (!code) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(MOD_MAP) / sizeof(MOD_MAP[0]); i++) {
        if (strcmp(MOD_MAP[i].code, code) == 0) {
            return MOD_MAP[i].mask;
        }
    }
    return 0;
}

static void hid_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!event) {
        return;
    }
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_mounted = true;
        set_last_error(NULL, ESP_OK);
        ESP_LOGI(TAG, "USB HID mounted by host on GPIO%d/GPIO%d",
                 CONFIG_SI_HID_USB_DM_GPIO, CONFIG_SI_HID_USB_DP_GPIO);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_mounted = false;
        ESP_LOGI(TAG, "USB HID unmounted");
        break;
    default:
        break;
    }
}

static bool hid_mounted(void)
{
    return s_installed && s_mounted && tud_mounted();
}

static bool wait_hid_ready(uint8_t instance, uint32_t timeout_ms)
{
    if (!hid_mounted()) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    do {
        if (tud_hid_n_ready(instance)) {
            return true;
        }
        if (timeout_ms == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) <= timeout);

    return tud_hid_n_ready(instance);
}

static bool send_keyboard_report(void)
{
    if (!wait_hid_ready(HID_ITF_KEYBOARD, 50)) {
        return false;
    }
    return tud_hid_n_keyboard_report(HID_ITF_KEYBOARD, 0, s_modifiers, s_keys);
}

static int8_t clamp_i8(int value);
static uint16_t clamp_abs(int value);

static bool send_mouse_report(int8_t x, int8_t y, int8_t wheel, int8_t pan, uint32_t timeout_ms)
{
    if (!wait_hid_ready(HID_ITF_MOUSE, timeout_ms)) {
        return false;
    }
    return tud_hid_n_mouse_report(HID_ITF_MOUSE, 0, s_mouse_buttons, x, y, wheel, pan);
}

static bool send_abs_pointer_report(uint32_t timeout_ms)
{
    if (!wait_hid_ready(HID_ITF_ABS_POINTER, timeout_ms)) {
        return false;
    }
    uint8_t report[5] = {
        s_abs_buttons,
        (uint8_t)(s_abs_x & 0xFF),
        (uint8_t)(s_abs_x >> 8),
        (uint8_t)(s_abs_y & 0xFF),
        (uint8_t)(s_abs_y >> 8),
    };
    return tud_hid_n_report(HID_ITF_ABS_POINTER, 0, report, sizeof(report));
}

static void send_mouse_counts(int dx, int dy, uint32_t timeout_ms)
{
    if (!hid_mounted()) {
        return;
    }
    (void)send_mouse_report(clamp_i8(dx), clamp_i8(dy), 0, 0, timeout_ms);
}

static int8_t clamp_i8(int value)
{
    if (value > 127) {
        return 127;
    }
    if (value < -127) {
        return -127;
    }
    return (int8_t)value;
}

static uint16_t clamp_abs(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > HID_ABS_MAX) {
        return HID_ABS_MAX;
    }
    return (uint16_t)value;
}

static int json_int(const cJSON *obj, const char *name, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static double json_double(const cJSON *obj, const char *name, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static const char *json_string(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

esp_err_t si_hid_init(void)
{
    if (!SI_HID_CFG_ENABLE) {
        set_last_error("USB HID disabled", ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_installed) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Installing TinyUSB HID device on FS GPIO%d/GPIO%d",
             CONFIG_SI_HID_USB_DM_GPIO, CONFIG_SI_HID_USB_DP_GPIO);
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(hid_event_cb, NULL);
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = HID_CONFIGURATION_DESCRIPTOR;
    tusb_cfg.descriptor.high_speed_config = NULL;
    tusb_cfg.descriptor.string = HID_STRING_DESCRIPTOR;
    tusb_cfg.descriptor.string_count = sizeof(HID_STRING_DESCRIPTOR) / sizeof(HID_STRING_DESCRIPTOR[0]);

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        set_last_error("install tinyusb", ret);
        return ret;
    }

    s_installed = true;
    set_last_error(NULL, ESP_OK);
    ESP_LOGI(TAG, "TinyUSB HID installed on Full-Speed USB");
    return ESP_OK;
}

bool si_hid_is_ready(void)
{
    return hid_mounted() &&
           tud_hid_n_ready(HID_ITF_KEYBOARD) &&
           tud_hid_n_ready(HID_ITF_MOUSE) &&
           tud_hid_n_ready(HID_ITF_ABS_POINTER);
}

bool si_hid_is_mounted(void)
{
    return hid_mounted();
}

void si_hid_get_status(si_hid_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->enabled = SI_HID_CFG_ENABLE;
    status->initialized = s_installed;
    status->mounted = si_hid_is_mounted();
    status->ready = si_hid_is_ready();
    status->dm_gpio = CONFIG_SI_HID_USB_DM_GPIO;
    status->dp_gpio = CONFIG_SI_HID_USB_DP_GPIO;
    status->tx_messages = s_tx_messages;
    status->failed_messages = s_failed_messages;
    strlcpy(status->mode, "usb-fs-gpio26-27", sizeof(status->mode));
    strlcpy(status->port, "USB FS GPIO26/27", sizeof(status->port));
    strlcpy(status->transport, "tinyusb", sizeof(status->transport));
    strlcpy(status->last_error, s_last_error, sizeof(status->last_error));
}

void si_hid_key_down(const char *code)
{
    uint8_t mod = js_code_to_mod(code);
    if (mod) {
        s_modifiers |= mod;
    } else {
        uint8_t hid = js_code_to_hid(code);
        if (hid) {
            for (uint8_t i = 0; i < s_key_count; i++) {
                if (s_keys[i] == hid) {
                    (void)send_keyboard_report();
                    return;
                }
            }
            if (s_key_count < sizeof(s_keys)) {
                s_keys[s_key_count++] = hid;
            }
        }
    }
    (void)send_keyboard_report();
}

void si_hid_key_up(const char *code)
{
    uint8_t mod = js_code_to_mod(code);
    if (mod) {
        s_modifiers &= (uint8_t)~mod;
    } else {
        uint8_t hid = js_code_to_hid(code);
        for (uint8_t i = 0; i < s_key_count; i++) {
            if (s_keys[i] == hid) {
                for (uint8_t j = i; j + 1 < s_key_count; j++) {
                    s_keys[j] = s_keys[j + 1];
                }
                s_key_count--;
                s_keys[s_key_count] = 0;
                break;
            }
        }
    }
    (void)send_keyboard_report();
}

void si_hid_release_all(void)
{
    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));
    (void)send_keyboard_report();
    s_mouse_buttons = 0;
    (void)send_mouse_report(0, 0, 0, 0, 50);
    s_abs_buttons = 0;
    (void)send_abs_pointer_report(50);
}

void si_hid_mouse_move(float dx, float dy)
{
    int mx = lroundf(dx * 127.0f);
    int my = lroundf(dy * 127.0f);
    send_mouse_counts(mx, my, 5);
}

static uint8_t browser_button_to_hid(uint8_t button)
{
    switch (button) {
    case 0:
        return MOUSE_BUTTON_LEFT;
    case 1:
        return MOUSE_BUTTON_MIDDLE;
    case 2:
        return MOUSE_BUTTON_RIGHT;
    default:
        return MOUSE_BUTTON_LEFT;
    }
}

void si_hid_mouse_down(uint8_t button)
{
    s_mouse_buttons |= browser_button_to_hid(button);
    (void)send_mouse_report(0, 0, 0, 0, 50);
}

void si_hid_mouse_up(uint8_t button)
{
    s_mouse_buttons &= (uint8_t)~browser_button_to_hid(button);
    (void)send_mouse_report(0, 0, 0, 0, 50);
}

void si_hid_mouse_scroll(int8_t delta_y, int8_t delta_x)
{
    (void)send_mouse_report(0, 0, delta_y, delta_x, 20);
}

void si_hid_abs_move(uint16_t x, uint16_t y)
{
    s_abs_x = clamp_abs(x);
    s_abs_y = clamp_abs(y);
    (void)send_abs_pointer_report(20);
}

void si_hid_abs_mouse_down(uint8_t button)
{
    s_abs_buttons |= browser_button_to_hid(button);
    (void)send_abs_pointer_report(50);
}

void si_hid_abs_mouse_up(uint8_t button)
{
    s_abs_buttons &= (uint8_t)~browser_button_to_hid(button);
    (void)send_abs_pointer_report(50);
}

static void send_combo(const cJSON *modifiers, const cJSON *keys)
{
    uint8_t prev_mod = s_modifiers;
    uint8_t prev_keys[6];
    uint8_t prev_count = s_key_count;
    memcpy(prev_keys, s_keys, sizeof(prev_keys));

    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, modifiers) {
        if (cJSON_IsString(item)) {
            s_modifiers |= js_code_to_mod(item->valuestring);
        }
    }
    cJSON_ArrayForEach(item, keys) {
        if (cJSON_IsString(item) && s_key_count < sizeof(s_keys)) {
            uint8_t hid = js_code_to_hid(item->valuestring);
            if (hid) {
                s_keys[s_key_count++] = hid;
            }
        }
    }

    (void)send_keyboard_report();
    vTaskDelay(pdMS_TO_TICKS(60));
    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));
    (void)send_keyboard_report();

    s_modifiers = prev_mod;
    s_key_count = prev_count;
    memcpy(s_keys, prev_keys, sizeof(s_keys));
}

esp_err_t si_hid_handle_json(const cJSON *msg)
{
    if (!cJSON_IsObject(msg)) {
        s_failed_messages++;
        set_last_error("invalid hid json", ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_installed) {
        s_failed_messages++;
        set_last_error("hid not initialized", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!hid_mounted()) {
        s_failed_messages++;
        set_last_error("hid not mounted", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    const char *type = json_string(msg, "type");
    if (!type) {
        s_failed_messages++;
        set_last_error("hid type missing", ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (strcmp(type, "keydown") == 0) {
        si_hid_key_down(json_string(msg, "code"));
    } else if (strcmp(type, "keyup") == 0) {
        si_hid_key_up(json_string(msg, "code"));
    } else if (strcmp(type, "mousemove") == 0) {
        const char *unit = json_string(msg, "unit");
        if (unit && (strcmp(unit, "hid") == 0 || strcmp(unit, "counts") == 0)) {
            send_mouse_counts(json_int(msg, "dx", json_int(msg, "x", 0)),
                              json_int(msg, "dy", json_int(msg, "y", 0)), 5);
        } else {
            double dx = json_double(msg, "dx", json_double(msg, "x", 0));
            double dy = json_double(msg, "dy", json_double(msg, "y", 0));
            si_hid_mouse_move((float)dx, (float)dy);
        }
    } else if (strcmp(type, "absmove") == 0) {
        si_hid_abs_move(clamp_abs(json_int(msg, "x", HID_ABS_MAX / 2)),
                        clamp_abs(json_int(msg, "y", HID_ABS_MAX / 2)));
    } else if (strcmp(type, "absmousedown") == 0) {
        si_hid_abs_mouse_down((uint8_t)json_int(msg, "button", 0));
    } else if (strcmp(type, "absmouseup") == 0) {
        si_hid_abs_mouse_up((uint8_t)json_int(msg, "button", 0));
    } else if (strcmp(type, "absclick") == 0) {
        uint8_t button = (uint8_t)json_int(msg, "button", 0);
        si_hid_abs_move(clamp_abs(json_int(msg, "x", s_abs_x)),
                        clamp_abs(json_int(msg, "y", s_abs_y)));
        si_hid_abs_mouse_down(button);
        vTaskDelay(pdMS_TO_TICKS(25));
        si_hid_abs_mouse_up(button);
    } else if (strcmp(type, "mousedown") == 0) {
        si_hid_mouse_down((uint8_t)json_int(msg, "button", 0));
    } else if (strcmp(type, "mouseup") == 0) {
        si_hid_mouse_up((uint8_t)json_int(msg, "button", 0));
    } else if (strcmp(type, "click") == 0) {
        uint8_t button = (uint8_t)json_int(msg, "button", 0);
        si_hid_mouse_down(button);
        vTaskDelay(pdMS_TO_TICKS(25));
        si_hid_mouse_up(button);
    } else if (strcmp(type, "wheel") == 0) {
        si_hid_mouse_scroll((int8_t)json_int(msg, "deltaY", 0),
                            (int8_t)json_int(msg, "deltaX", 0));
    } else if (strcmp(type, "combo") == 0) {
        send_combo(cJSON_GetObjectItemCaseSensitive(msg, "modifiers"),
                   cJSON_GetObjectItemCaseSensitive(msg, "keys"));
    } else if (strcmp(type, "releaseall") == 0) {
        si_hid_release_all();
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        s_tx_messages++;
        set_last_error(NULL, ESP_OK);
    } else {
        s_failed_messages++;
        set_last_error("unsupported hid command", ret);
    }
    return ret;
}
