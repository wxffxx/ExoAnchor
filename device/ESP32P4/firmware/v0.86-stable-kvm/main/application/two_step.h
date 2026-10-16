#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SI_TWO_STEP_CODE_MAX 32
#define SI_TWO_STEP_RECOVERY_COUNT 8

typedef struct {
    bool enabled;
    bool storage_ok;
    bool time_ready;
    unsigned recovery_remaining;
} si_two_step_status_t;

typedef struct {
    char id[33];
    char secret[33];
    char recovery[SI_TWO_STEP_RECOVERY_COUNT][33];
} si_two_step_enrollment_t;

esp_err_t si_two_step_initialize(void);
void si_two_step_clock_start(void);
void si_two_step_get_status(si_two_step_status_t *out);
bool si_two_step_verify(const char *code, uint32_t credential_generation);
esp_err_t si_two_step_begin(uint32_t generation, si_two_step_enrollment_t *out);
esp_err_t si_two_step_confirm(const char *id, const char *code, uint32_t generation);
esp_err_t si_two_step_disable(const char *code, uint32_t generation);
