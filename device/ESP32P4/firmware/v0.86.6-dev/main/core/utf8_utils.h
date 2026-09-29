#pragma once

#include <stddef.h>

// Returns a malloc-owned, valid UTF-8 copy. Invalid bytes become '?'.
char *si_utf8_sanitize(const char *content, size_t max_len);
