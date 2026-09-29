#include "boot_key_sequence.h"

#include <ctype.h>
#include <string.h>

static bool safe_profile_id(const char *value)
{
    if (!value || !value[0]) {
        return false;
    }
    size_t len = strlen(value);
    if (len > SI_BOOT_KEY_PROFILE_ID_MAX ||
        !isalnum((unsigned char)value[0])) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

static bool allowed_boot_key(const char *key_code)
{
    static const char *const allowed[] = {
        "Delete", "Escape", "F1", "F2", "F3", "F4", "F5", "F6",
        "F7", "F8", "F9", "F10", "F11", "F12",
    };
    if (!key_code || !key_code[0] ||
        strlen(key_code) > SI_BOOT_KEY_CODE_MAX) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(key_code, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool si_boot_key_sequence_validate(
    const si_boot_key_sequence_config_t *config)
{
    if (!config || !safe_profile_id(config->profile_id) ||
        !allowed_boot_key(config->key_code) ||
        (config->target != SI_BOOT_KEY_TARGET_BIOS_SETUP &&
         config->target != SI_BOOT_KEY_TARGET_BOOT_MENU) ||
        (config->trigger != SI_BOOT_KEY_TRIGGER_NONE &&
         config->trigger != SI_BOOT_KEY_TRIGGER_RESET) ||
        config->start_delay_ms > 10000U ||
        config->interval_ms < 100U || config->interval_ms > 5000U ||
        config->max_attempts < 1U || config->max_attempts > 20U ||
        config->total_timeout_ms < 1000U ||
        config->total_timeout_ms > 60000U) {
        return false;
    }

    uint32_t last_due = config->start_delay_ms +
        ((uint32_t)config->max_attempts - 1U) * config->interval_ms;
    return last_due <= UINT32_MAX - 100U &&
           config->total_timeout_ms >= last_due + 100U;
}

si_boot_key_decision_t si_boot_key_sequence_decide(
    const si_boot_key_sequence_config_t *config, uint32_t elapsed_ms,
    uint8_t attempts_sent, bool cancel_requested, bool human_kvm_active,
    bool lease_held, bool hid_ready)
{
    if (!si_boot_key_sequence_validate(config)) {
        return SI_BOOT_KEY_DECISION_CANCEL;
    }
    if (cancel_requested) {
        return SI_BOOT_KEY_DECISION_CANCEL;
    }
    if (human_kvm_active || !lease_held) {
        return SI_BOOT_KEY_DECISION_PREEMPT;
    }
    if (attempts_sent >= config->max_attempts) {
        return SI_BOOT_KEY_DECISION_COMPLETE;
    }
    if (elapsed_ms >= config->total_timeout_ms) {
        return SI_BOOT_KEY_DECISION_TIMEOUT;
    }
    if (!hid_ready) {
        return SI_BOOT_KEY_DECISION_WAIT_HID;
    }

    uint32_t next_due = config->start_delay_ms +
        (uint32_t)attempts_sent * config->interval_ms;
    return elapsed_ms >= next_due ? SI_BOOT_KEY_DECISION_SEND :
                                    SI_BOOT_KEY_DECISION_WAIT;
}

const char *si_boot_key_target_name(si_boot_key_target_t target)
{
    switch (target) {
    case SI_BOOT_KEY_TARGET_BIOS_SETUP:
        return "bios_setup";
    case SI_BOOT_KEY_TARGET_BOOT_MENU:
        return "boot_menu";
    default:
        return "invalid";
    }
}

const char *si_boot_key_trigger_name(si_boot_key_trigger_t trigger)
{
    switch (trigger) {
    case SI_BOOT_KEY_TRIGGER_NONE:
        return "none";
    case SI_BOOT_KEY_TRIGGER_RESET:
        return "reset";
    default:
        return "invalid";
    }
}

const char *si_boot_key_decision_name(si_boot_key_decision_t decision)
{
    switch (decision) {
    case SI_BOOT_KEY_DECISION_WAIT:
        return "wait";
    case SI_BOOT_KEY_DECISION_WAIT_HID:
        return "wait_hid";
    case SI_BOOT_KEY_DECISION_SEND:
        return "send";
    case SI_BOOT_KEY_DECISION_COMPLETE:
        return "complete";
    case SI_BOOT_KEY_DECISION_CANCEL:
        return "cancel";
    case SI_BOOT_KEY_DECISION_PREEMPT:
        return "preempt";
    case SI_BOOT_KEY_DECISION_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}
