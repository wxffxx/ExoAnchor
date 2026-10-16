#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define SI_PASSKEY_ORIGIN_MAX 269
#define SI_PASSKEY_LIMIT 4
#define SI_PASSKEY_ID_MAX 256

esp_err_t si_passkey_initialize(void);
bool si_passkey_origin_valid(const char *origin);
/* Returned JSON is owned by the caller; never contains private keys. */
cJSON *si_passkey_status(bool include_credentials);
esp_err_t si_passkey_configure(const char *origin, uint32_t generation);
esp_err_t si_passkey_remove(const char *id, uint32_t generation);
esp_err_t si_passkey_begin(bool registration, const char *origin,
                           uint32_t generation, const char *session_id,
                           const char *label, cJSON **options);
esp_err_t si_passkey_finish(bool registration, const char *origin,
                            const char *session_id, const cJSON *response,
                            uint32_t *accepted_generation);

/* Bounded-depth, complete object parser rejecting duplicate keys/NULs. */
cJSON *si_passkey_parse_json(const char *text);
bool si_passkey_session_binding(const char *token, char out[44]);
