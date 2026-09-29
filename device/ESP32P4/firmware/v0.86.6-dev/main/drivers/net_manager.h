#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "network_config.h"

typedef struct {
    bool configured;
    bool initialized;
    bool connected;
    bool link_up;
    bool full_duplex;
    bool pending_confirmation;
    bool recovered_pending;
    bool recovery_active;
    int retry_count;
    int speed_mbps;
    uint32_t config_generation;
    uint32_t confirm_remaining_seconds;
    char interface[16];
    char driver[32];
    char device_id[32];
    char hostname[64];
    char mode[12];
    char address_source[12];
    char config_state[16];
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns_primary[16];
    char dns_secondary[16];
    char mac[18];
    char last_error[96];
} si_net_status_t;

esp_err_t si_net_init(void);
void si_net_get_status(si_net_status_t *out);
esp_err_t si_net_get_active_config(si_network_config_t *out);
esp_err_t si_net_get_staged_config(si_network_config_t *out);
esp_err_t si_net_stage_config(const si_network_config_t *config);
esp_err_t si_net_apply_staged(void);
esp_err_t si_net_commit_pending(void);
esp_err_t si_net_rollback_pending(void);
esp_err_t si_net_reset_config(void);
