#pragma once

#include <stddef.h>

// Returns a malloc-owned, valid UTF-8 copy. Invalid bytes become '?'.
char *si_utf8_sanitize(const char *content, size_t max_len);
size_t si_utf8_prefix_len(const char *text, size_t len, size_t max_len);
void si_utf8_strlcpy(char *dst, const char *src, size_t dst_size);
