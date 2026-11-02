#include "echo_redact.h"

#include <string.h>

static void wipe(void *data, size_t len)
{
    volatile uint8_t *p = data;
    while (len--) {
        *p++ = 0;
    }
}

void si_echo_redact_disarm(si_echo_redact_t *redact)
{
    if (redact) {
        wipe(redact, sizeof(*redact));
    }
}

void si_echo_redact_arm(si_echo_redact_t *redact, const uint8_t *secret,
                        size_t len)
{
    if (!redact) {
        return;
    }
    si_echo_redact_disarm(redact);
    if (!secret || len == 0U || len > SI_ECHO_REDACT_MAX) {
        return;
    }
    memcpy(redact->secret, secret, len);
    redact->len = len;
    size_t k = 0;
    redact->fail[0] = 0;
    for (size_t i = 1; i < len; i++) {
        while (k > 0 && secret[i] != secret[k]) {
            k = redact->fail[k - 1U];
        }
        if (secret[i] == secret[k]) {
            k++;
        }
        redact->fail[i] = (uint16_t)k;
    }
}

bool si_echo_redact_armed(const si_echo_redact_t *redact)
{
    return redact && redact->len > 0U;
}

size_t si_echo_redact_feed(si_echo_redact_t *redact, const uint8_t *in,
                           size_t len, uint8_t *out)
{
    if (!in || !out || len == 0U) {
        return 0;
    }
    if (!si_echo_redact_armed(redact)) {
        memcpy(out, in, len);
        return len;
    }
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        size_t held = redact->state;
        size_t next = held;
        while (next > 0 && redact->secret[next] != c) {
            next = redact->fail[next - 1U];
        }
        if (redact->secret[next] == c) {
            next++;
        }
        if (next == redact->len) {
            memset(out + out_len, '*', redact->len);
            out_len += redact->len;
            redact->state = 0;
            continue;
        }
        size_t released = held + 1U - next;
        for (size_t k = 0; k < released; k++) {
            out[out_len++] = k < held ? redact->secret[k] : c;
        }
        redact->state = next;
    }
    return out_len;
}

size_t si_echo_redact_flush(si_echo_redact_t *redact, uint8_t *out)
{
    if (!si_echo_redact_armed(redact) || !out) {
        return 0;
    }
    size_t held = redact->state;
    memset(out, '*', held);
    redact->state = 0;
    return held;
}
