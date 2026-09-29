#pragma once

#include "esp_err.h"

#define SI_NETWORK_DISCOVERY_PORT 39393U

esp_err_t si_network_discovery_start(void);
