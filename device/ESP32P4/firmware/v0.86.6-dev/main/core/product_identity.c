#include "product_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool label_valid(const char *value, size_t max_len)
{
    if (!value) {
        return false;
    }
    size_t len = strlen(value);
    if (len == 0 || len > max_len || value[0] == '-' ||
        value[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (!(islower(ch) || isdigit(ch) || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool si_product_hostname_valid(const char *hostname)
{
    return label_valid(hostname, SI_PRODUCT_HOSTNAME_MAX_LEN);
}

bool si_product_device_id_valid(const char *device_id)
{
    return label_valid(device_id, SI_PRODUCT_DEVICE_ID_MAX_LEN);
}

void si_product_identity_default_from_mac(const uint8_t mac[6],
                                          si_product_identity_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!mac) {
        return;
    }
    snprintf(out->suffix, sizeof(out->suffix), "%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    snprintf(out->device_id, sizeof(out->device_id), "ea-p4-%s", out->suffix);
    snprintf(out->hostname, sizeof(out->hostname), "exoanchor-%s", out->suffix);
}
