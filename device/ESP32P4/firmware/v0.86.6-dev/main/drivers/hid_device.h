#pragma once

// USB HID device public API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_owner.h"

#define SI_HID_ABS_MAX 32767
#define SI_HID_COMBO_MODIFIER_MAX 8
#define SI_HID_COMBO_KEY_MAX 6

typedef enum {
    SI_HID_COMMAND_INVALID = 0,
    SI_HID_COMMAND_UNSUPPORTED,
    SI_HID_COMMAND_KEY_DOWN,
    SI_HID_COMMAND_KEY_UP,
    SI_HID_COMMAND_MOUSE_MOVE,
    SI_HID_COMMAND_MOUSE_MOVE_COUNTS,
    SI_HID_COMMAND_ABS_MOVE,
    SI_HID_COMMAND_ABS_MOUSE_DOWN,
    SI_HID_COMMAND_ABS_MOUSE_UP,
    SI_HID_COMMAND_ABS_CLICK,
    SI_HID_COMMAND_MOUSE_DOWN,
    SI_HID_COMMAND_MOUSE_UP,
    SI_HID_COMMAND_CLICK,
    SI_HID_COMMAND_WHEEL,
    SI_HID_COMMAND_COMBO,
    SI_HID_COMMAND_RELEASE_ALL,
} si_hid_command_type_t;

typedef struct {
    si_hid_command_type_t type;
    const char *code;
    const char *error;
    float dx;
    float dy;
    int32_t x;
    int32_t y;
    bool has_x;
    bool has_y;
    uint8_t button;
    int8_t wheel_y;
    int8_t wheel_x;
    const char *modifiers[SI_HID_COMBO_MODIFIER_MAX];
    size_t modifier_count;
    const char *keys[SI_HID_COMBO_KEY_MAX];
    size_t key_count;
} si_hid_command_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool mounted;
    bool ready;
    bool keyboard_writable;
    bool mouse_writable;
    bool absolute_pointer_writable;
    int dm_gpio;
    int dp_gpio;
    uint32_t tx_messages;
    uint32_t failed_messages;
    char mode[32];
    char port[32];
    char transport[24];
    char last_error[96];
} si_hid_status_t;

/*
 * Called while the HID manager's authority gate is held, immediately before
 * the driver command is admitted.  Implementations may take short-lived auth,
 * video, and control-lease locks, but must not call a blocking HID command.
 */
typedef si_hid_live_guard_fn si_hid_authority_guard_fn;

typedef struct {
    bool authorize;
    bool replace;
    si_hid_owner_claim_t claim;
} si_hid_owner_transition_t;

/* Called under the single HID authority gate.  Domain callbacks may take
 * their own short-lived lock, but must never emit HID reports. */
typedef bool (*si_hid_owner_transition_fn)(
    void *context, const si_hid_owner_token_t *current,
    si_hid_owner_transition_t *transition);

esp_err_t si_hid_init(void);
bool si_hid_is_ready(void);
bool si_hid_is_mounted(void);
void si_hid_get_status(si_hid_status_t *status);

esp_err_t si_hid_execute_owned(const si_hid_command_t *command,
                               const si_hid_owner_token_t *owner,
                               si_hid_authority_guard_fn guard,
                               void *guard_context);

/* Legacy embedded producers are admitted only while this manager-level guard
 * confirms the live embedded control lease.  Registration is single-owner and
 * fail-closed: ordinary execute/release APIs reject every report until set. */
esp_err_t si_hid_set_embedded_authority_guard(
    si_hid_authority_guard_fn guard, void *guard_context);
esp_err_t si_hid_bind_embedded_producer(
    const si_hid_owner_token_t *owner);
bool si_hid_get_bound_embedded_owner(si_hid_owner_token_t *owner);

bool si_hid_authority_transition(si_hid_owner_transition_fn transition,
                                 void *context,
                                 si_hid_owner_token_t *owner);
bool si_hid_owner_release_if_current(const si_hid_owner_token_t *owner);
bool si_hid_owner_revoke_session(const char *session_id,
                                 uint32_t auth_generation);
bool si_hid_owner_get_current(si_hid_owner_token_t *owner);

/* Transitional Agent task epilogue: release the token bound to this exact
 * producer task.  Callers without a binding are a no-op. */
void si_hid_release_all(void);
