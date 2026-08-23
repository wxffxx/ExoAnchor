#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_PRODUCT_DEVICE_ID_MAX_LEN 31U
#define SI_PRODUCT_HOSTNAME_MAX_LEN 63U
#define SI_PRODUCT_SUFFIX_LEN 6U

typedef struct {
    char device_id[SI_PRODUCT_DEVICE_ID_MAX_LEN + 1];
    char hostname[SI_PRODUCT_HOSTNAME_MAX_LEN + 1];
    char suffix[SI_PRODUCT_SUFFIX_LEN + 1];
} si_product_identity_t;

bool si_product_hostname_valid(const char *hostname);
bool si_product_device_id_valid(const char *device_id);
void si_product_identity_default_from_mac(const uint8_t mac[6],
                                          si_product_identity_t *out);
