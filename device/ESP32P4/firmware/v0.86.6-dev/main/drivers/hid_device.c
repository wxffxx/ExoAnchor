// USB HID device driver.
#include "hid_device.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_abort_fence.h"
#include "hid_owner.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "si-hid";

#define HID_ITF_KEYBOARD 0
#define HID_ITF_MOUSE 1
#define HID_ITF_ABS_POINTER 2
#define HID_EP_KEYBOARD 0x81
#define HID_EP_MOUSE 0x82
#define HID_EP_ABS_POINTER 0x83
#define HID_EP_SIZE 8
#define HID_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN)
#define HID_EMBEDDED_PRODUCER_SLOTS 8U

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
    SI_CFG_HID_MANUFACTURER,
    SI_CFG_HID_PRODUCT,
    SI_CFG_HID_SERIAL,
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
static uint16_t s_abs_x = SI_HID_ABS_MAX / 2;
static uint16_t s_abs_y = SI_HID_ABS_MAX / 2;
static uint32_t s_tx_messages;
static uint32_t s_failed_messages;
static char s_last_error[96];
/*
 * Lock order is authority gate -> short-lived auth/video/lease locks ->
 * command lock.  The recursive gate is required because a live-auth lookup
 * may expire a session and synchronously enter the revocation path on the
 * same task.  The gate is released once command admission is complete, so a
 * revoker can advance the abort fence while a composite report is delayed.
 */
static SemaphoreHandle_t s_authority_gate;
static SemaphoreHandle_t s_command_lock;
static si_hid_abort_fence_t s_abort_fence =
    SI_HID_ABORT_FENCE_INITIALIZER;
static si_hid_owner_state_t s_owner_state =
    SI_HID_OWNER_STATE_INITIALIZER;
static uint32_t s_active_command_generation;
static si_hid_owner_token_t s_active_command_owner;
static bool s_command_active;
static bool s_command_was_aborted;
static bool s_command_send_ok;
static const char *s_command_send_error;
/* A USB detach/attach re-enumerates the host-side HID state. Apply that
 * reset under the HID manager locks instead of pre-queuing neutral reports
 * to endpoints that Linux has not opened yet (notably mouse on a tty). */
static bool s_mount_reset_pending = true;
static si_hid_authority_guard_fn s_embedded_authority_guard;
static void *s_embedded_authority_guard_context;
typedef struct {
    TaskHandle_t task;
    si_hid_owner_token_t owner;
} embedded_producer_binding_t;
static embedded_producer_binding_t
    s_embedded_producers[HID_EMBEDDED_PRODUCER_SLOTS];

static esp_err_t ensure_manager_locks(void)
{
    if (!s_authority_gate) {
        s_authority_gate = xSemaphoreCreateRecursiveMutex();
        if (!s_authority_gate) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_command_lock) {
        s_command_lock = xSemaphoreCreateMutex();
        if (!s_command_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void abort_pending_commands(void)
{
    (void)si_hid_abort_fence_advance(&s_abort_fence);
}

static bool active_command_aborted(void)
{
    return s_command_active &&
           !si_hid_abort_fence_is_current(
               &s_abort_fence, s_active_command_generation);
}

static bool active_command_can_emit(void)
{
    if (!active_command_aborted()) {
        return true;
    }
    s_command_send_ok = false;
    s_command_send_error = "HID command authority revoked";
    return false;
}

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
        __atomic_store_n(&s_mount_reset_pending, true, __ATOMIC_RELEASE);
        set_last_error(NULL, ESP_OK);
        ESP_LOGI(TAG, "USB HID mounted by host on GPIO%d/GPIO%d",
                 SI_CFG_HID_DM_GPIO, SI_CFG_HID_DP_GPIO);
        break;
    case TINYUSB_EVENT_DETACHED:
        s_mounted = false;
        __atomic_store_n(&s_mount_reset_pending, true, __ATOMIC_RELEASE);
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
    if (!hid_mounted() || !active_command_can_emit()) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    do {
        if (!active_command_can_emit()) {
            return false;
        }
        if (tud_hid_n_ready(instance)) {
            return true;
        }
        if (timeout_ms == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) <= timeout);

    return active_command_can_emit() && tud_hid_n_ready(instance);
}

static bool send_keyboard_report(void)
{
    if (!active_command_can_emit() ||
        !wait_hid_ready(HID_ITF_KEYBOARD, 50) ||
        !active_command_can_emit()) {
        return false;
    }
    const bool ok = tud_hid_n_keyboard_report(
        HID_ITF_KEYBOARD, 0, s_modifiers, s_keys);
    if (ok) {
        si_hid_owner_state_note_report(
            &s_owner_state, SI_HID_REPORT_DIRTY_KEYBOARD,
            s_modifiers == 0U && s_key_count == 0U);
    }
    return ok;
}

static int8_t clamp_i8(int value);
static uint16_t clamp_abs(int value);

static bool send_mouse_report(int8_t x, int8_t y, int8_t wheel, int8_t pan, uint32_t timeout_ms)
{
    if (!active_command_can_emit() ||
        !wait_hid_ready(HID_ITF_MOUSE, timeout_ms) ||
        !active_command_can_emit()) {
        return false;
    }
    const bool ok = tud_hid_n_mouse_report(
        HID_ITF_MOUSE, 0, s_mouse_buttons, x, y, wheel, pan);
    if (ok) {
        si_hid_owner_state_note_report(
            &s_owner_state, SI_HID_REPORT_DIRTY_MOUSE,
            s_mouse_buttons == 0U);
    }
    return ok;
}

static bool send_abs_pointer_report(uint32_t timeout_ms)
{
    if (!active_command_can_emit() ||
        !wait_hid_ready(HID_ITF_ABS_POINTER, timeout_ms) ||
        !active_command_can_emit()) {
        return false;
    }
    uint8_t report[5] = {
        s_abs_buttons,
        (uint8_t)(s_abs_x & 0xFF),
        (uint8_t)(s_abs_x >> 8),
        (uint8_t)(s_abs_y & 0xFF),
        (uint8_t)(s_abs_y >> 8),
    };
    const bool ok = tud_hid_n_report(
        HID_ITF_ABS_POINTER, 0, report, sizeof(report));
    if (ok) {
        si_hid_owner_state_note_report(
            &s_owner_state, SI_HID_REPORT_DIRTY_ABS_POINTER,
            s_abs_buttons == 0U);
    }
    return ok;
}

static void record_send_result(bool ok, const char *error)
{
    s_command_send_ok = s_command_send_ok && ok;
    if (!ok && !s_command_send_error) {
        s_command_send_error = error;
    }
}

static void send_mouse_counts(int dx, int dy, uint32_t timeout_ms)
{
    if (!hid_mounted()) {
        s_command_send_ok = false;
        return;
    }
    record_send_result(send_mouse_report(clamp_i8(dx), clamp_i8(dy),
                                         0, 0, timeout_ms),
                       "relative mouse endpoint not writable");
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
    if (value > SI_HID_ABS_MAX) {
        return SI_HID_ABS_MAX;
    }
    return (uint16_t)value;
}

esp_err_t si_hid_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_manager_locks(), TAG,
                        "create HID execution manager locks");
    if (!SI_CFG_HID_ENABLED) {
        set_last_error("USB HID disabled", ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_installed) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Installing TinyUSB HID device on FS GPIO%d/GPIO%d",
             SI_CFG_HID_DM_GPIO, SI_CFG_HID_DP_GPIO);
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
    /*
     * tud_hid_n_ready() is an instantaneous endpoint-writable state. It can
     * legitimately be false while a previous report is in flight, so using
     * all three endpoints as the public readiness signal makes an otherwise
     * healthy mounted HID device flicker between ready/not-ready. Each send
     * path still waits for its own endpoint and reports a precise failure.
     */
    return hid_mounted();
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
    status->enabled = SI_CFG_HID_ENABLED;
    status->initialized = s_installed;
    status->mounted = si_hid_is_mounted();
    status->ready = si_hid_is_ready();
    status->keyboard_writable =
        status->mounted && tud_hid_n_ready(HID_ITF_KEYBOARD);
    status->mouse_writable =
        status->mounted && tud_hid_n_ready(HID_ITF_MOUSE);
    status->absolute_pointer_writable =
        status->mounted && tud_hid_n_ready(HID_ITF_ABS_POINTER);
    status->dm_gpio = SI_CFG_HID_DM_GPIO;
    status->dp_gpio = SI_CFG_HID_DP_GPIO;
    status->tx_messages = s_tx_messages;
    status->failed_messages = s_failed_messages;
    snprintf(status->mode, sizeof(status->mode), "usb-fs-gpio%d-%d",
             SI_CFG_HID_DM_GPIO, SI_CFG_HID_DP_GPIO);
    snprintf(status->port, sizeof(status->port), "USB FS GPIO%d/%d",
             SI_CFG_HID_DM_GPIO, SI_CFG_HID_DP_GPIO);
    strlcpy(status->transport, "tinyusb", sizeof(status->transport));
    strlcpy(status->last_error, s_last_error, sizeof(status->last_error));
}

static void hid_key_down(const char *code)
{
    uint8_t mod = js_code_to_mod(code);
    if (mod) {
        s_modifiers |= mod;
    } else {
        uint8_t hid = js_code_to_hid(code);
        if (hid) {
            for (uint8_t i = 0; i < s_key_count; i++) {
                if (s_keys[i] == hid) {
                    record_send_result(send_keyboard_report(),
                                       "keyboard endpoint not writable");
                    return;
                }
            }
            if (s_key_count < sizeof(s_keys)) {
                s_keys[s_key_count++] = hid;
            }
        }
    }
    record_send_result(send_keyboard_report(),
                       "keyboard endpoint not writable");
}

static void hid_key_up(const char *code)
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
    record_send_result(send_keyboard_report(),
                       "keyboard endpoint not writable");
}

static void release_all_locked(void)
{
    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));
    if (s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_KEYBOARD) {
        record_send_result(send_keyboard_report(),
                           "keyboard endpoint not writable");
    }
    s_mouse_buttons = 0;
    if (s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_MOUSE) {
        record_send_result(send_mouse_report(0, 0, 0, 0, 50),
                           "relative mouse endpoint not writable");
    }
    s_abs_buttons = 0;
    if (s_owner_state.dirty_reports & SI_HID_REPORT_DIRTY_ABS_POINTER) {
        record_send_result(send_abs_pointer_report(50),
                           "absolute pointer endpoint not writable");
    }
}

static void reset_report_state_locked(void)
{
    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));
    s_mouse_buttons = 0;
    s_abs_buttons = 0;
}

/* authority gate and command lock are both held */
static bool apply_mount_reset_locked(void)
{
    if (!__atomic_exchange_n(&s_mount_reset_pending, false,
                             __ATOMIC_ACQ_REL)) {
        return false;
    }
    /* USB re-enumeration discards reports held by the previous host
     * configuration. Never poison keyboard ownership by queueing neutral
     * reports to a Linux mouse endpoint that is not open yet. */
    reset_report_state_locked();
    si_hid_owner_state_host_reset(&s_owner_state);
    return true;
}

/* authority gate and command lock are both held */
static bool drain_neutral_pending_locked(void)
{
    if (apply_mount_reset_locked()) {
        return true;
    }
    reset_report_state_locked();
    if (!hid_mounted()) {
        /* A detached USB function cannot retain reports at the host. */
        si_hid_owner_state_host_reset(&s_owner_state);
        return true;
    }
    s_command_active = false;
    s_command_was_aborted = false;
    s_command_send_ok = true;
    s_command_send_error = NULL;
    release_all_locked();
    s_owner_state.neutral_pending =
        !s_command_send_ok || s_owner_state.dirty_reports != 0U;
    if (!s_owner_state.neutral_pending) {
        memset(&s_owner_state.report_owner, 0,
               sizeof(s_owner_state.report_owner));
    }
    return !s_owner_state.neutral_pending;
}

static bool neutralize_aborted_command_locked(void)
{
    if (s_command_was_aborted) {
        return true;
    }
    if (!active_command_aborted()) {
        return false;
    }

    /*
     * Do not let the fence suppress the neutral reports themselves.  The
     * command lock remains held, so no later command can interleave before
     * this local release or the revoker's serialized release.
     */
    s_command_was_aborted = true;
    s_command_active = false;
    s_command_send_ok = true;
    s_command_send_error = "HID command authority revoked";
    release_all_locked();
    return true;
}

static bool owner_transition_locked(const si_hid_owner_transition_t *transition,
                                    si_hid_owner_token_t *owner)
{
    const bool install = transition && transition->authorize;
    if (install && !si_hid_owner_claim_valid(&transition->claim)) {
        return false;
    }
    if (install && !transition->replace &&
        s_owner_state.current.generation != 0U &&
        si_hid_owner_claim_equal(&s_owner_state.current.claim,
                                 &transition->claim)) {
        if (owner) {
            *owner = s_owner_state.current;
        }
        return true;
    }

    si_hid_owner_token_t previous = s_owner_state.current;
    const bool had_previous = previous.generation != 0U;
    if (had_previous) {
        (void)si_hid_owner_state_revoke_current(
            &s_owner_state, &previous, NULL);
        for (size_t i = 0; i < HID_EMBEDDED_PRODUCER_SLOTS; i++) {
            if (si_hid_owner_token_equal(
                    &s_embedded_producers[i].owner, &previous)) {
                memset(&s_embedded_producers[i], 0,
                       sizeof(s_embedded_producers[i]));
            }
        }
    }
    abort_pending_commands();
    if (xSemaphoreTake(s_command_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    (void)apply_mount_reset_locked();
    const bool previous_owned_reports = had_previous &&
        si_hid_owner_token_equal(&s_owner_state.report_owner, &previous);
    bool neutral = true;
    if (previous_owned_reports || s_owner_state.neutral_pending) {
        neutral = drain_neutral_pending_locked();
    }
    xSemaphoreGive(s_command_lock);
    if (!neutral && hid_mounted()) {
        return false;
    }
    if (install && !si_hid_owner_state_install(
            &s_owner_state, &transition->claim, true, owner)) {
        return false;
    }
    if (!install && owner) {
        memset(owner, 0, sizeof(*owner));
    }
    return true;
}

bool si_hid_authority_transition(si_hid_owner_transition_fn callback,
                                 void *context,
                                 si_hid_owner_token_t *owner)
{
    if (!callback || ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    si_hid_owner_transition_t transition = {0};
    bool accepted = callback(context, &s_owner_state.current, &transition);
    bool complete = accepted && owner_transition_locked(&transition, owner);
    xSemaphoreGiveRecursive(s_authority_gate);
    return complete;
}

bool si_hid_owner_release_if_current(const si_hid_owner_token_t *owner)
{
    if (!owner || ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool current = si_hid_owner_state_is_current(&s_owner_state, owner);
    si_hid_owner_transition_t transition = {0};
    bool complete = current && owner_transition_locked(&transition, NULL);
    xSemaphoreGiveRecursive(s_authority_gate);
    return complete;
}

bool si_hid_owner_revoke_session(const char *session_id,
                                 uint32_t auth_generation)
{
    if (!session_id || !session_id[0] || auth_generation == 0U ||
        ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const si_hid_owner_token_t current = s_owner_state.current;
    bool matches = current.generation != 0U &&
        strcmp(current.claim.session_id, session_id) == 0 &&
        current.claim.auth_generation == auth_generation;
    si_hid_owner_transition_t transition = {0};
    bool complete = matches && owner_transition_locked(&transition, NULL);
    xSemaphoreGiveRecursive(s_authority_gate);
    return complete;
}

bool si_hid_owner_get_current(si_hid_owner_token_t *owner)
{
    if (!owner || ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, pdMS_TO_TICKS(100)) !=
            pdTRUE) {
        return false;
    }
    *owner = s_owner_state.current;
    bool present = owner->generation != 0U;
    xSemaphoreGiveRecursive(s_authority_gate);
    return present;
}

esp_err_t si_hid_set_embedded_authority_guard(
    si_hid_authority_guard_fn guard, void *guard_context)
{
    if (!guard || ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    if (s_embedded_authority_guard &&
        (s_embedded_authority_guard != guard ||
         s_embedded_authority_guard_context != guard_context)) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_embedded_authority_guard = guard;
        s_embedded_authority_guard_context = guard_context;
    }
    xSemaphoreGiveRecursive(s_authority_gate);
    return ret;
}

esp_err_t si_hid_bind_embedded_producer(
    const si_hid_owner_token_t *owner)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (!owner || !task ||
        owner->claim.kind != SI_HID_OWNER_CONTROL_LEASE ||
        owner->claim.auth_generation != 0U ||
        strcmp(owner->claim.session_id, "embedded-agent") != 0 ||
        ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!si_hid_owner_state_is_current(&s_owner_state, owner)) {
        xSemaphoreGiveRecursive(s_authority_gate);
        return ESP_ERR_INVALID_STATE;
    }
    size_t slot = HID_EMBEDDED_PRODUCER_SLOTS;
    for (size_t i = 0; i < HID_EMBEDDED_PRODUCER_SLOTS; i++) {
        if (s_embedded_producers[i].task == task) {
            slot = i;
            break;
        }
        if (slot == HID_EMBEDDED_PRODUCER_SLOTS &&
            !s_embedded_producers[i].task) {
            slot = i;
        }
    }
    if (slot == HID_EMBEDDED_PRODUCER_SLOTS) {
        xSemaphoreGiveRecursive(s_authority_gate);
        return ESP_ERR_NO_MEM;
    }
    s_embedded_producers[slot].task = task;
    s_embedded_producers[slot].owner = *owner;
    xSemaphoreGiveRecursive(s_authority_gate);
    return ESP_OK;
}

bool si_hid_get_bound_embedded_owner(si_hid_owner_token_t *owner)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (!owner || !task || ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate, pdMS_TO_TICKS(100)) !=
            pdTRUE) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < HID_EMBEDDED_PRODUCER_SLOTS; i++) {
        if (s_embedded_producers[i].task == task) {
            *owner = s_embedded_producers[i].owner;
            found = true;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_authority_gate);
    return found;
}

static void hid_mouse_move(float dx, float dy)
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

static void hid_mouse_down(uint8_t button)
{
    s_mouse_buttons |= browser_button_to_hid(button);
    record_send_result(send_mouse_report(0, 0, 0, 0, 50),
                       "relative mouse endpoint not writable");
}

static void hid_mouse_up(uint8_t button)
{
    s_mouse_buttons &= (uint8_t)~browser_button_to_hid(button);
    record_send_result(send_mouse_report(0, 0, 0, 0, 50),
                       "relative mouse endpoint not writable");
}

static void hid_mouse_scroll(int8_t delta_y, int8_t delta_x)
{
    record_send_result(send_mouse_report(0, 0, delta_y, delta_x, 20),
                       "relative mouse endpoint not writable");
}

static void hid_abs_move(uint16_t x, uint16_t y)
{
    s_abs_x = clamp_abs(x);
    s_abs_y = clamp_abs(y);
    record_send_result(send_abs_pointer_report(20),
                       "absolute pointer endpoint not writable");
}

static void hid_abs_mouse_down(uint8_t button)
{
    s_abs_buttons |= browser_button_to_hid(button);
    record_send_result(send_abs_pointer_report(50),
                       "absolute pointer endpoint not writable");
}

static void hid_abs_mouse_up(uint8_t button)
{
    s_abs_buttons &= (uint8_t)~browser_button_to_hid(button);
    record_send_result(send_abs_pointer_report(50),
                       "absolute pointer endpoint not writable");
}

static void send_combo(const si_hid_command_t *command)
{
    uint8_t prev_mod = s_modifiers;
    uint8_t prev_keys[6];
    uint8_t prev_count = s_key_count;
    memcpy(prev_keys, s_keys, sizeof(prev_keys));

    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));

    for (size_t i = 0; i < command->modifier_count; i++) {
        s_modifiers |= js_code_to_mod(command->modifiers[i]);
    }
    for (size_t i = 0; i < command->key_count && s_key_count < sizeof(s_keys); i++) {
        uint8_t hid = js_code_to_hid(command->keys[i]);
        if (hid) {
            s_keys[s_key_count++] = hid;
        }
    }

    record_send_result(send_keyboard_report(),
                       "keyboard endpoint not writable");
    if (neutralize_aborted_command_locked()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    /* A revoke may run while the composite command is delayed.  Never
     * restore the pre-combo report after its abort generation changes. */
    if (neutralize_aborted_command_locked()) {
        return;
    }
    s_modifiers = 0;
    s_key_count = 0;
    memset(s_keys, 0, sizeof(s_keys));
    record_send_result(send_keyboard_report(),
                       "keyboard endpoint not writable");
    if (neutralize_aborted_command_locked()) {
        return;
    }

    s_modifiers = prev_mod;
    s_key_count = prev_count;
    memcpy(s_keys, prev_keys, sizeof(s_keys));
    record_send_result(send_keyboard_report(),
                       "keyboard endpoint not writable");
}

esp_err_t si_hid_execute_owned(const si_hid_command_t *command,
                               const si_hid_owner_token_t *owner,
                               si_hid_authority_guard_fn guard,
                               void *guard_context)
{
    if (!command || !owner || command->type == SI_HID_COMMAND_INVALID) {
        s_failed_messages++;
        set_last_error(command && command->error ? command->error : "invalid hid command",
                       ESP_ERR_INVALID_ARG);
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
    if (ensure_manager_locks() != ESP_OK ||
        xSemaphoreTakeRecursive(s_authority_gate,
                                pdMS_TO_TICKS(500)) != pdTRUE) {
        s_failed_messages++;
        set_last_error("hid authority gate busy", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    /*
     * This is the command-admission linearization boundary.  The live
     * session/capability/lease check and command-lock acquisition occur under
     * the same gate used by revoke/release.  A lock-free generation catches
     * an expiry or KVM/lease invalidation that occurs through another state
     * manager while the guard is running.
     */
    if (!si_hid_owner_state_is_current(&s_owner_state, owner)) {
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("stale HID report owner", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t admission_generation =
        si_hid_abort_fence_snapshot(&s_abort_fence);
    bool embedded_owner =
        owner->claim.kind == SI_HID_OWNER_CONTROL_LEASE &&
        owner->claim.auth_generation == 0U &&
        strcmp(owner->claim.session_id, "embedded-agent") == 0;
    if ((embedded_owner &&
         (!s_embedded_authority_guard ||
          !s_embedded_authority_guard(
              s_embedded_authority_guard_context))) ||
        (guard && !guard(guard_context))) {
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("hid command authority denied", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_command_lock ||
        xSemaphoreTake(s_command_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("hid command busy", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    if (!si_hid_abort_fence_is_current(
            &s_abort_fence, admission_generation)) {
        xSemaphoreGive(s_command_lock);
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("hid authority changed before command start",
                       ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if ((__atomic_load_n(&s_mount_reset_pending, __ATOMIC_ACQUIRE) ||
         s_owner_state.neutral_pending) &&
        !drain_neutral_pending_locked()) {
        xSemaphoreGive(s_command_lock);
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("HID neutral reports pending", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!si_hid_owner_state_begin_report(&s_owner_state, owner)) {
        xSemaphoreGive(s_command_lock);
        xSemaphoreGiveRecursive(s_authority_gate);
        s_failed_messages++;
        set_last_error("HID report owner changed", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    s_active_command_generation = admission_generation;
    s_active_command_owner = *owner;
    s_command_active = true;
    s_command_was_aborted = false;
    s_command_send_ok = true;
    s_command_send_error = NULL;
    xSemaphoreGiveRecursive(s_authority_gate);

    switch (command->type) {
    case SI_HID_COMMAND_KEY_DOWN:
        hid_key_down(command->code);
        break;
    case SI_HID_COMMAND_KEY_UP:
        hid_key_up(command->code);
        break;
    case SI_HID_COMMAND_MOUSE_MOVE:
        hid_mouse_move(command->dx, command->dy);
        break;
    case SI_HID_COMMAND_MOUSE_MOVE_COUNTS:
        send_mouse_counts(command->x, command->y, 5);
        break;
    case SI_HID_COMMAND_ABS_MOVE:
        hid_abs_move(clamp_abs(command->x), clamp_abs(command->y));
        break;
    case SI_HID_COMMAND_ABS_MOUSE_DOWN:
        hid_abs_mouse_down(command->button);
        break;
    case SI_HID_COMMAND_ABS_MOUSE_UP:
        hid_abs_mouse_up(command->button);
        break;
    case SI_HID_COMMAND_ABS_CLICK:
        hid_abs_move(command->has_x ? clamp_abs(command->x) : s_abs_x,
                     command->has_y ? clamp_abs(command->y) : s_abs_y);
        hid_abs_mouse_down(command->button);
        if (neutralize_aborted_command_locked()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
        if (neutralize_aborted_command_locked()) {
            break;
        }
        hid_abs_mouse_up(command->button);
        break;
    case SI_HID_COMMAND_MOUSE_DOWN:
        hid_mouse_down(command->button);
        break;
    case SI_HID_COMMAND_MOUSE_UP:
        hid_mouse_up(command->button);
        break;
    case SI_HID_COMMAND_CLICK:
        hid_mouse_down(command->button);
        if (neutralize_aborted_command_locked()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
        if (neutralize_aborted_command_locked()) {
            break;
        }
        hid_mouse_up(command->button);
        break;
    case SI_HID_COMMAND_WHEEL:
        hid_mouse_scroll(command->wheel_y, command->wheel_x);
        break;
    case SI_HID_COMMAND_COMBO:
        send_combo(command);
        break;
    case SI_HID_COMMAND_RELEASE_ALL:
        release_all_locked();
        if (s_command_send_ok) {
            si_hid_owner_state_clear_report(&s_owner_state, owner);
        } else {
            s_owner_state.neutral_pending = true;
        }
        break;
    case SI_HID_COMMAND_UNSUPPORTED:
    case SI_HID_COMMAND_INVALID:
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    if (neutralize_aborted_command_locked() || s_command_was_aborted) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (ret == ESP_OK && !s_command_send_ok) {
        ret = ESP_FAIL;
    }
    const char *terminal_send_error = s_command_send_error;
    s_command_active = false;
    if (ret == ESP_OK && s_owner_state.dirty_reports == 0U) {
        si_hid_owner_state_clear_report(&s_owner_state, owner);
    }
    if (ret == ESP_FAIL && !s_command_send_ok) {
        /* A failed tail report (notably click/combo key/button-up) may leave
         * the host holding input.  Neutralize now; if any endpoint remains
         * unwritable, neutral_pending prevents every later report/owner. */
        s_owner_state.neutral_pending =
            s_owner_state.dirty_reports != 0U;
        if (s_owner_state.neutral_pending) {
            (void)drain_neutral_pending_locked();
        }
        s_command_send_error = terminal_send_error;
    }
    memset(&s_active_command_owner, 0, sizeof(s_active_command_owner));

    if (ret == ESP_OK) {
        s_tx_messages++;
        set_last_error(NULL, ESP_OK);
    } else {
        s_failed_messages++;
        set_last_error(command->error ? command->error :
                       (s_command_send_error ? s_command_send_error :
                        "HID report send failed"),
                       ret);
    }
    xSemaphoreGive(s_command_lock);
    return ret;
}

void si_hid_release_all(void)
{
    si_hid_owner_token_t owner = {0};
    if (!si_hid_get_bound_embedded_owner(&owner) ||
        owner.claim.kind != SI_HID_OWNER_CONTROL_LEASE ||
        owner.claim.auth_generation != 0U ||
        strcmp(owner.claim.session_id, "embedded-agent") != 0) {
        return;
    }
    const si_hid_command_t command = {
        .type = SI_HID_COMMAND_RELEASE_ALL,
    };
    (void)si_hid_execute_owned(&command, &owner, NULL, NULL);
}
