#pragma once

#include <stdbool.h>

typedef struct {
    const char *code;
    bool shift;
} si_hid_ascii_key_t;

/*
 * Map one printable US-ASCII character to the JavaScript KeyboardEvent code
 * used by the HID driver. This deliberately excludes control bytes and UTF-8:
 * a stored Console credential must be reproducible on the target keyboard.
 */
bool si_hid_ascii_map(unsigned char value, si_hid_ascii_key_t *key);
