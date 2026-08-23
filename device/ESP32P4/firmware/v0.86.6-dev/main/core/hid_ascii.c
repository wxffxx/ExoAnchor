#include "hid_ascii.h"

#include <stddef.h>

typedef struct {
    unsigned char value;
    const char *code;
    bool shift;
} ascii_mapping_t;

static const char *const DIGIT_CODES[] = {
    "Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
    "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
};

static const char *const LETTER_CODES[] = {
    "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG",
    "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN",
    "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU",
    "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ",
};

static const ascii_mapping_t SYMBOLS[] = {
    {' ', "Space", false},
    {'-', "Minus", false}, {'_', "Minus", true},
    {'=', "Equal", false}, {'+', "Equal", true},
    {'[', "BracketLeft", false}, {'{', "BracketLeft", true},
    {']', "BracketRight", false}, {'}', "BracketRight", true},
    {'\\', "Backslash", false}, {'|', "Backslash", true},
    {';', "Semicolon", false}, {':', "Semicolon", true},
    {'\'', "Quote", false}, {'"', "Quote", true},
    {'`', "Backquote", false}, {'~', "Backquote", true},
    {',', "Comma", false}, {'<', "Comma", true},
    {'.', "Period", false}, {'>', "Period", true},
    {'/', "Slash", false}, {'?', "Slash", true},
    {'!', "Digit1", true}, {'@', "Digit2", true},
    {'#', "Digit3", true}, {'$', "Digit4", true},
    {'%', "Digit5", true}, {'^', "Digit6", true},
    {'&', "Digit7", true}, {'*', "Digit8", true},
    {'(', "Digit9", true}, {')', "Digit0", true},
};

bool si_hid_ascii_map(unsigned char value, si_hid_ascii_key_t *key)
{
    if (!key) {
        return false;
    }
    key->code = NULL;
    key->shift = false;

    if (value >= '0' && value <= '9') {
        key->code = DIGIT_CODES[value - '0'];
        return true;
    }
    if (value >= 'a' && value <= 'z') {
        key->code = LETTER_CODES[value - 'a'];
        return true;
    }
    if (value >= 'A' && value <= 'Z') {
        key->code = LETTER_CODES[value - 'A'];
        key->shift = true;
        return true;
    }
    for (size_t index = 0;
         index < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); index++) {
        if (SYMBOLS[index].value == value) {
            key->code = SYMBOLS[index].code;
            key->shift = SYMBOLS[index].shift;
            return true;
        }
    }
    return false;
}
