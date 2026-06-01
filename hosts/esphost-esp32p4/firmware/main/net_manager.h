#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    bool configured;
    bool connected;
    bool link_up;
    int retry_count;
    char interface[16];
    char driver[24];
    char ip[16];
    char netmask[16];
    char gateway[16];
    char mac[18];
} si_net_status_t;

esp_err_t si_net_init(void);
void si_net_get_status(si_net_status_t *out);
