#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Return a newly allocated checkpoint JSON document with durable page context
 * removed.  All other Run recovery fields, including the original message,
 * remain unchanged.  The caller owns *json_out.
 */
esp_err_t si_agent_page_context_strip_checkpoint(const char *json,
                                                 char **json_out,
                                                 bool *changed_out);
