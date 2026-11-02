#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_ECHO_REDACT_MAX 128U

typedef struct {
    uint8_t secret[SI_ECHO_REDACT_MAX];
    uint16_t fail[SI_ECHO_REDACT_MAX];
    size_t len;
    size_t state;
} si_echo_redact_t;

void si_echo_redact_arm(si_echo_redact_t *redact, const uint8_t *secret,
                        size_t len);
void si_echo_redact_disarm(si_echo_redact_t *redact);
bool si_echo_redact_armed(const si_echo_redact_t *redact);
size_t si_echo_redact_feed(si_echo_redact_t *redact, const uint8_t *in,
                           size_t len, uint8_t *out);
size_t si_echo_redact_flush(si_echo_redact_t *redact, uint8_t *out);
