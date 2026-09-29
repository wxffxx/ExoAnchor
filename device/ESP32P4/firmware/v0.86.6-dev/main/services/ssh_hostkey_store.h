#pragma once

// Cache-disable-safe persistence for SSH trust-on-first-use host keys.

#include <stdbool.h>

#include "esp_err.h"

#define SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN 64

bool si_ssh_hostkey_record_valid(const char *fingerprint_hex);
bool si_ssh_hostkey_record_matches(const char *saved,
                                   const char *observed);

/*
 * Verify an existing TOFU record or persist the first observed fingerprint.
 * The implementation copies every NVS input/output into internal DRAM and
 * executes the complete transaction on a short-lived internal-stack task, so
 * this API is safe to call from the PSRAM-backed SSH workers.
 */
esp_err_t si_ssh_hostkey_verify_or_trust(const char *key,
                                         const char *fingerprint_hex,
                                         bool *trusted_new_out);
