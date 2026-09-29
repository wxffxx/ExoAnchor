#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SI_BOOT_KEY_PROFILE_ID_MAX 31U
#define SI_BOOT_KEY_CODE_MAX 15U

typedef enum {
    SI_BOOT_KEY_TARGET_INVALID = 0,
    SI_BOOT_KEY_TARGET_BIOS_SETUP,
    SI_BOOT_KEY_TARGET_BOOT_MENU,
} si_boot_key_target_t;

typedef enum {
    SI_BOOT_KEY_TRIGGER_NONE = 0,
    SI_BOOT_KEY_TRIGGER_RESET,
} si_boot_key_trigger_t;

typedef struct {
    char profile_id[SI_BOOT_KEY_PROFILE_ID_MAX + 1U];
    si_boot_key_target_t target;
    si_boot_key_trigger_t trigger;
    char key_code[SI_BOOT_KEY_CODE_MAX + 1U];
    uint32_t start_delay_ms;
    uint32_t interval_ms;
    uint8_t max_attempts;
    uint32_t total_timeout_ms;
} si_boot_key_sequence_config_t;

typedef enum {
    SI_BOOT_KEY_DECISION_WAIT = 0,
    SI_BOOT_KEY_DECISION_WAIT_HID,
    SI_BOOT_KEY_DECISION_SEND,
    SI_BOOT_KEY_DECISION_COMPLETE,
    SI_BOOT_KEY_DECISION_CANCEL,
    SI_BOOT_KEY_DECISION_PREEMPT,
    SI_BOOT_KEY_DECISION_TIMEOUT,
} si_boot_key_decision_t;

bool si_boot_key_sequence_validate(
    const si_boot_key_sequence_config_t *config);

si_boot_key_decision_t si_boot_key_sequence_decide(
    const si_boot_key_sequence_config_t *config, uint32_t elapsed_ms,
    uint8_t attempts_sent, bool cancel_requested, bool human_kvm_active,
    bool lease_held, bool hid_ready);

const char *si_boot_key_target_name(si_boot_key_target_t target);
const char *si_boot_key_trigger_name(si_boot_key_trigger_t trigger);
const char *si_boot_key_decision_name(si_boot_key_decision_t decision);
