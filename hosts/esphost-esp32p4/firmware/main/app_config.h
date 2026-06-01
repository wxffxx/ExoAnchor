#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#define SI_BMC_VERSION "0.2.0"
#define SI_BMC_HOSTNAME "si-bmc-p4"
#define SI_BMC_BOARD "Waveshare ESP32-P4-NANO"

#define SI_STATUS_POLL_INTERVAL_MS 1000U
#define SI_DEFAULT_HTTP_PORT 80

static inline bool si_auth_enabled(void)
{
    return CONFIG_SI_AUTH_PASSWORD[0] != '\0';
}
