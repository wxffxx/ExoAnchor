#include "network_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (!error || error_size == 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

bool si_network_ipv4_parse(const char *text, uint32_t *host_order_out)
{
    if (!text || !text[0]) {
        return false;
    }
    uint32_t parts[4] = {0};
    const char *cursor = text;
    for (size_t i = 0; i < 4; ++i) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        uint32_t value = 0;
        size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (uint32_t)(*cursor - '0');
            if (++digits > 3 || value > 255U) {
                return false;
            }
            cursor++;
        }
        parts[i] = value;
        if (i < 3) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    if (host_order_out) {
        *host_order_out = (parts[0] << 24) | (parts[1] << 16) |
                          (parts[2] << 8) | parts[3];
    }
    return true;
}
static bool ipv4_unicast(uint32_t address, bool allow_autoip)
{
    uint8_t first = (uint8_t)(address >> 24);
    uint8_t second = (uint8_t)(address >> 16);
    if (first == 0 || first == 127 || first >= 224) {
        return false;
    }
    if (!allow_autoip && first == 169 && second == 254) {
        return false;
    }
    return address != 0xffffffffU;
}

static bool netmask_contiguous(uint32_t mask)
{
    if (mask == 0) {
        return false;
    }
    uint32_t inverse = ~mask;
    return (inverse & (inverse + 1U)) == 0;
}

void si_network_config_default(si_network_config_t *out,
                               const char *default_hostname)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->schema_version = SI_NETWORK_CONFIG_SCHEMA_VERSION;
    out->mode = SI_NETWORK_MODE_DHCP;
    out->autoip_fallback = true;
    if (si_product_hostname_valid(default_hostname)) {
        snprintf(out->hostname, sizeof(out->hostname), "%s", default_hostname);
    }
}

const char *si_network_mode_name(si_network_mode_t mode)
{
    switch (mode) {
    case SI_NETWORK_MODE_DHCP:
        return "dhcp";
    case SI_NETWORK_MODE_STATIC:
        return "static";
    default:
        return "unknown";
    }
}

bool si_network_config_validate(const si_network_config_t *config,
                                char *error, size_t error_size)
{
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!config) {
        set_error(error, error_size, "config is required");
        return false;
    }
    if (config->schema_version != SI_NETWORK_CONFIG_SCHEMA_VERSION) {
        set_error(error, error_size, "unsupported schema version");
        return false;
    }
    if (!si_product_hostname_valid(config->hostname)) {
        set_error(error, error_size, "hostname is not a valid RFC1123 label");
        return false;
    }
    if (config->mode != SI_NETWORK_MODE_DHCP &&
        config->mode != SI_NETWORK_MODE_STATIC) {
        set_error(error, error_size, "network mode is invalid");
        return false;
    }
    if (config->mode == SI_NETWORK_MODE_DHCP) {
        return true;
    }

    uint32_t address = 0;
    uint32_t netmask = 0;
    if (!si_network_ipv4_parse(config->address, &address) ||
        !ipv4_unicast(address, false)) {
        set_error(error, error_size, "static IPv4 address is invalid");
        return false;
    }
    if (!si_network_ipv4_parse(config->netmask, &netmask) ||
        !netmask_contiguous(netmask)) {
        set_error(error, error_size, "static IPv4 netmask is invalid");
        return false;
    }
    uint32_t host_bits = address & ~netmask;
    if (host_bits == 0 || host_bits == ~netmask) {
        set_error(error, error_size, "static IPv4 is network or broadcast");
        return false;
    }

    if (config->gateway[0]) {
        uint32_t gateway = 0;
        if (!si_network_ipv4_parse(config->gateway, &gateway) ||
            !ipv4_unicast(gateway, false) ||
            (gateway & netmask) != (address & netmask)) {
            set_error(error, error_size,
                      "gateway must be a unicast address in the subnet");
            return false;
        }
    }
    const char *dns_values[] = {
        config->dns_primary,
        config->dns_secondary,
    };
    for (size_t i = 0; i < sizeof(dns_values) / sizeof(dns_values[0]); ++i) {
        if (!dns_values[i][0]) {
            continue;
        }
        uint32_t dns = 0;
        if (!si_network_ipv4_parse(dns_values[i], &dns) ||
            !ipv4_unicast(dns, true)) {
            set_error(error, error_size, "DNS IPv4 address is invalid");
            return false;
        }
    }
    return true;
}
