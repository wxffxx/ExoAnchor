#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "product_identity.h"

#define SI_NETWORK_CONFIG_SCHEMA_VERSION 1U
#define SI_NETWORK_IPV4_TEXT_MAX_LEN 15U
#define SI_NETWORK_VALIDATION_ERROR_MAX_LEN 96U

typedef enum {
    SI_NETWORK_MODE_DHCP = 0,
    SI_NETWORK_MODE_STATIC = 1,
} si_network_mode_t;

typedef struct {
    uint32_t schema_version;
    uint32_t generation;
    si_network_mode_t mode;
    bool autoip_fallback;
    char hostname[SI_PRODUCT_HOSTNAME_MAX_LEN + 1];
    char address[SI_NETWORK_IPV4_TEXT_MAX_LEN + 1];
    char netmask[SI_NETWORK_IPV4_TEXT_MAX_LEN + 1];
    char gateway[SI_NETWORK_IPV4_TEXT_MAX_LEN + 1];
    char dns_primary[SI_NETWORK_IPV4_TEXT_MAX_LEN + 1];
    char dns_secondary[SI_NETWORK_IPV4_TEXT_MAX_LEN + 1];
} si_network_config_t;

void si_network_config_default(si_network_config_t *out,
                               const char *default_hostname);
bool si_network_config_validate(const si_network_config_t *config,
                                char *error, size_t error_size);
const char *si_network_mode_name(si_network_mode_t mode);
bool si_network_ipv4_parse(const char *text, uint32_t *host_order_out);
