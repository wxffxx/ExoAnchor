#include "utf8_utils.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool continuation(uint8_t ch) { return (ch & 0xc0U) == 0x80U; }

static bool valid_sequence(const uint8_t *s, size_t available, size_t *length)
{
    uint8_t c = s[0];
    if (c < 0x80U) { *length = 1; return true; }
    if (c >= 0xc2U && c <= 0xdfU && available >= 2 && continuation(s[1])) { *length = 2; return true; }
    if (c == 0xe0U && available >= 3 && s[1] >= 0xa0U && s[1] <= 0xbfU && continuation(s[2])) { *length = 3; return true; }
    if (((c >= 0xe1U && c <= 0xecU) || (c >= 0xeeU && c <= 0xefU)) &&
        available >= 3 && continuation(s[1]) && continuation(s[2])) { *length = 3; return true; }
    if (c == 0xedU && available >= 3 && s[1] >= 0x80U && s[1] <= 0x9fU && continuation(s[2])) { *length = 3; return true; }
    if (c == 0xf0U && available >= 4 && s[1] >= 0x90U && s[1] <= 0xbfU &&
        continuation(s[2]) && continuation(s[3])) { *length = 4; return true; }
    if (c >= 0xf1U && c <= 0xf3U && available >= 4 && continuation(s[1]) &&
        continuation(s[2]) && continuation(s[3])) { *length = 4; return true; }
    if (c == 0xf4U && available >= 4 && s[1] >= 0x80U && s[1] <= 0x8fU &&
        continuation(s[2]) && continuation(s[3])) { *length = 4; return true; }
    return false;
}

char *si_utf8_sanitize(const char *content, size_t max_len)
{
    if (!content) return NULL;
    size_t input_len = strlen(content);
    size_t limit = max_len && input_len > max_len ? max_len : input_len;
    size_t capacity = limit + 16;
    char *out = calloc(1, capacity + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    bool truncated = input_len > limit;
    while (i < limit && o + 5 < capacity) {
        uint8_t ch = (uint8_t)content[i];
        if (ch < 0x80U) {
            out[o++] = (ch < 0x20U && ch != '\n' && ch != '\r' && ch != '\t') ? ' ' : (char)ch;
            ++i;
            continue;
        }
        size_t length = 0;
        if (valid_sequence((const uint8_t *)content + i, input_len - i, &length) && i + length <= limit) {
            memcpy(out + o, content + i, length); o += length; i += length;
        } else { out[o++] = '?'; ++i; }
    }
    if (max_len && input_len > i) truncated = true;
    static const char suffix[] = "\n[truncated]";
    if (truncated && o + sizeof(suffix) < capacity) {
        memcpy(out + o, suffix, sizeof(suffix) - 1); o += sizeof(suffix) - 1;
    }
    out[o] = 0;
    return out;
}
