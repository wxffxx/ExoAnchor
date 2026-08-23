#include "net_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/acd.h"
#include "lwip/ip4_addr.h"
#include "network_settings.h"
#include "product_identity.h"

static const char *TAG = "si-net";

#define ETH_DRIVER_INSTALL_ATTEMPTS 3
#define ETH_DRIVER_INSTALL_RETRY_DELAY_MS 250
#define NETWORK_MANAGER_INTERVAL_MS 250
#define NETWORK_LINK_REFRESH_MS 1000
#define NETWORK_CONFIRM_TIMEOUT_SECONDS 120U
#define NETWORK_STATIC_ACD_TIMEOUT_SECONDS 12U
#define NETWORK_SETTINGS_WORKER_STACK 3072U

typedef enum {
    STATIC_ACD_IDLE = 0,
    STATIC_ACD_WAITING,
    STATIC_ACD_OK,
    STATIC_ACD_CONFLICT,
    STATIC_ACD_TIMEOUT,
} static_acd_result_t;

static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;
static si_net_status_t s_status;
static si_network_config_t s_factory_config;
static si_network_config_t s_runtime_config;
static si_product_identity_t s_identity;
static SemaphoreHandle_t s_status_lock;
static SemaphoreHandle_t s_operation_lock;
static TaskHandle_t s_manager_task;
static int64_t s_pending_deadline_us;
static int64_t s_static_acd_deadline_us;
static int s_logged_speed_mbps;
static bool s_logged_full_duplex;
static struct acd s_static_acd;
static volatile static_acd_result_t s_static_acd_result;
static bool s_static_acd_registered;
static esp_err_t s_settings_worker_result;
static UBaseType_t s_settings_worker_stack_high_water;

static void status_lock(void)
{
    if (s_status_lock) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_status_lock) {
        xSemaphoreGive(s_status_lock);
    }
}

static bool current_task_stack_is_internal(void)
{
    volatile uint8_t stack_probe = 0;
    return esp_ptr_in_dram((const void *)&stack_probe)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast((const void *)&stack_probe)
#endif
        ;
}

static void network_settings_rollback_task(void *opaque)
{
    TaskHandle_t caller = (TaskHandle_t)opaque;
    esp_err_t result = si_network_settings_rollback();
    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    __atomic_store_n(&s_settings_worker_result, result, __ATOMIC_RELEASE);
    __atomic_store_n(&s_settings_worker_stack_high_water, stack_high_water,
                     __ATOMIC_RELEASE);
    xTaskNotifyGive(caller);
    /* The manager performs the non-self WithCaps deletion. This avoids IDF's
     * temporary cleanup-task allocation (and abort path) for self deletion. */
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static esp_err_t rollback_settings_on_internal_stack(void)
{
    if (current_task_stack_is_internal()) {
        return si_network_settings_rollback();
    }

    /* Only si_net_mgr uses this dedicated notification handshake. Other
     * external-stack callers keep the existing fail-closed behavior instead
     * of sharing a task-notification slot with unrelated diagnostics work. */
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (caller != s_manager_task) {
        return ESP_ERR_INVALID_STATE;
    }

    /* si_net_mgr is intentionally PSRAM-backed in H.264 builds so the idle
     * Agent admission baseline retains its 24 KiB internal executor stack.
     * NVS cannot disable the external-memory cache while that task is on the
     * call stack, so perform only the rare persistence step on an ephemeral
     * short-lived internal worker. No dynamic cross-task context survives the
     * call, so SMP wakeup cannot race a context/semaphore free. */
    (void)ulTaskNotifyTake(pdTRUE, 0);
    TaskHandle_t worker = NULL;
    BaseType_t created = xTaskCreateWithCaps(
        network_settings_rollback_task, "si_net_nvs",
        NETWORK_SETTINGS_WORKER_STACK, (void *)caller,
        tskIDLE_PRIORITY + 5, &worker,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* This replaces the former synchronous NVS call, so completion is not an
     * additional unbounded dependency. The worker always signals after the
     * single rollback transaction, including its error path. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_err_t result = __atomic_load_n(
        &s_settings_worker_result, __ATOMIC_ACQUIRE);
    UBaseType_t stack_high_water = __atomic_load_n(
        &s_settings_worker_stack_high_water, __ATOMIC_ACQUIRE);
    /* This is deliberately non-self: IDF can reclaim the explicit-cap stack
     * synchronously without creating its low-memory-sensitive cleanup task. */
    vTaskDeleteWithCaps(worker);
    ESP_LOGI(TAG, "network settings internal worker min high-water=%u",
             (unsigned)stack_high_water);
    return result;
}

static void set_last_error(esp_err_t error, const char *context)
{
    status_lock();
    if (error == ESP_OK) {
        s_status.last_error[0] = '\0';
    } else {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s",
                 context ? context : "network",
                 esp_err_to_name(error));
    }
    status_unlock();
}

static void static_acd_callback(struct netif *netif,
                                acd_callback_enum_t state)
{
    (void)netif;
    if (s_static_acd_result != STATIC_ACD_WAITING) {
        return;
    }
    if (state == ACD_IP_OK) {
        s_static_acd_result = STATIC_ACD_OK;
    } else if (state == ACD_DECLINE || state == ACD_RESTART_CLIENT) {
        s_static_acd_result = STATIC_ACD_CONFLICT;
    }
}

typedef struct {
    ip4_addr_t address;
} static_acd_start_context_t;

static esp_err_t static_acd_start_tcpip(void *opaque)
{
    static_acd_start_context_t *context = opaque;
    struct netif *netif = esp_netif_get_netif_impl(s_eth_netif);
    if (!context || !netif) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_static_acd, 0, sizeof(s_static_acd));
    err_t err = acd_add(netif, &s_static_acd, static_acd_callback);
    if (err != ERR_OK) {
        return ESP_FAIL;
    }
    s_static_acd_registered = true;
    err = acd_start(netif, &s_static_acd, context->address);
    if (err != ERR_OK) {
        acd_remove(netif, &s_static_acd);
        s_static_acd_registered = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t static_acd_stop_tcpip(void *opaque)
{
    (void)opaque;
    struct netif *netif = esp_netif_get_netif_impl(s_eth_netif);
    if (s_static_acd_registered && netif) {
        acd_remove(netif, &s_static_acd);
    }
    s_static_acd_registered = false;
    return ESP_OK;
}

static void stop_static_acd(void)
{
    if (s_static_acd_registered) {
        (void)esp_netif_tcpip_exec(static_acd_stop_tcpip, NULL);
    }
    s_static_acd_deadline_us = 0;
    s_static_acd_result = STATIC_ACD_IDLE;
}

static esp_err_t start_static_acd(const si_network_config_t *config)
{
    if (!config || config->mode != SI_NETWORK_MODE_STATIC ||
        s_static_acd_result != STATIC_ACD_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    static_acd_start_context_t context = {0};
    if (!ip4addr_aton(config->address, &context.address)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_static_acd_result = STATIC_ACD_WAITING;
    esp_err_t ret = esp_netif_tcpip_exec(static_acd_start_tcpip, &context);
    if (ret != ESP_OK) {
        stop_static_acd();
        return ret;
    }
    s_static_acd_deadline_us =
        esp_timer_get_time() +
        (int64_t)NETWORK_STATIC_ACD_TIMEOUT_SECONDS * 1000000LL;
    return ESP_OK;
}

static void copy_ip(char *dst, size_t dst_size, const esp_ip4_addr_t *addr)
{
    snprintf(dst, dst_size, IPSTR, IP2STR(addr));
}

static void clear_ip_status_locked(void)
{
    s_status.ip[0] = '\0';
    s_status.netmask[0] = '\0';
    s_status.gateway[0] = '\0';
    s_status.address_source[0] = '\0';
}

static void refresh_ip_status_for_mode_locked(
    const esp_netif_ip_info_t *ip_info, si_network_mode_t mode)
{
    if (!ip_info) {
        return;
    }
    copy_ip(s_status.ip, sizeof(s_status.ip), &ip_info->ip);
    copy_ip(s_status.netmask, sizeof(s_status.netmask), &ip_info->netmask);
    copy_ip(s_status.gateway, sizeof(s_status.gateway), &ip_info->gw);
    if (mode == SI_NETWORK_MODE_STATIC) {
        strlcpy(s_status.address_source, "static",
                sizeof(s_status.address_source));
    } else if (strncmp(s_status.ip, "169.254.", 8) == 0) {
        strlcpy(s_status.address_source, "autoip",
                sizeof(s_status.address_source));
    } else {
        strlcpy(s_status.address_source, "dhcp",
                sizeof(s_status.address_source));
    }
}

static void refresh_ip_status_locked(const esp_netif_ip_info_t *ip_info)
{
    refresh_ip_status_for_mode_locked(ip_info, s_runtime_config.mode);
}

static void update_config_status_locked(const si_network_config_t *config,
                                        const char *state)
{
    if (!config) {
        return;
    }
    s_status.config_generation = config->generation;
    strlcpy(s_status.hostname, config->hostname, sizeof(s_status.hostname));
    strlcpy(s_status.mode, si_network_mode_name(config->mode),
            sizeof(s_status.mode));
    strlcpy(s_status.dns_primary, config->dns_primary,
            sizeof(s_status.dns_primary));
    strlcpy(s_status.dns_secondary, config->dns_secondary,
            sizeof(s_status.dns_secondary));
    strlcpy(s_status.config_state, state, sizeof(s_status.config_state));
}

static void copy_eth_mac(void)
{
    uint8_t mac[6] = {0};
    if (!s_eth_handle ||
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) != ESP_OK) {
        return;
    }
    status_lock();
    snprintf(s_status.mac, sizeof(s_status.mac),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    status_unlock();
}

static void refresh_link_properties(void)
{
    bool link_up = false;
    bool static_mode = false;
    status_lock();
    link_up = s_status.link_up;
    static_mode = strcmp(s_status.mode, "static") == 0;
    status_unlock();
    if (!s_eth_handle || !link_up) {
        status_lock();
        s_status.speed_mbps = 0;
        s_status.full_duplex = false;
        status_unlock();
        return;
    }

    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    esp_err_t speed_ret =
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed);
    esp_err_t duplex_ret =
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
    status_lock();
    if (speed_ret == ESP_OK) {
        s_status.speed_mbps = speed == ETH_SPEED_100M ? 100 : 10;
    }
    if (duplex_ret == ESP_OK) {
        s_status.full_duplex = duplex == ETH_DUPLEX_FULL;
    }
    int current_speed = s_status.speed_mbps;
    bool current_duplex = s_status.full_duplex;
    status_unlock();
    if (duplex_ret != ESP_OK) {
        ESP_LOGW(TAG, "read Ethernet duplex mode failed: %s",
                 esp_err_to_name(duplex_ret));
    }
    if (speed_ret == ESP_OK && duplex_ret == ESP_OK &&
        (current_speed != s_logged_speed_mbps ||
         current_duplex != s_logged_full_duplex)) {
        ESP_LOGI(TAG, "Ethernet link: %dMbps %s-duplex",
                 current_speed, current_duplex ? "full" : "half");
        s_logged_speed_mbps = current_speed;
        s_logged_full_duplex = current_duplex;
    }

    /*
     * A static address does not emit the DHCP "got IP" event.  Refresh it
     * after link-up so status/discovery do not remain falsely disconnected
     * when the address was applied before the PHY negotiated a link.
     */
    if (static_mode && s_eth_netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            status_lock();
            s_status.connected = true;
            refresh_ip_status_locked(&ip_info);
            status_unlock();
        }
    }
}

static esp_err_t stop_dhcp_if_needed(void)
{
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_err_t ret = esp_netif_dhcpc_get_status(s_eth_netif, &status);
    if (ret != ESP_OK) {
        return ret;
    }
    /*
     * ESP_NETIF_DEFAULT_ETH creates the client in DHCP_INIT.  Static IPv4
     * still requires the client to transition to DHCP_STOPPED before
     * esp_netif_set_ip_info(), including during cold boot before the Ethernet
     * driver has started.
     */
    if (status != ESP_NETIF_DHCP_STOPPED) {
        ret = esp_netif_dhcpc_stop(s_eth_netif);
        if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ret = ESP_OK;
        }
    }
    return ret;
}

static esp_err_t set_dns(esp_netif_dns_type_t type, const char *value)
{
    if (!value || !value[0]) {
        return ESP_OK;
    }
    esp_netif_dns_info_t dns = {
        .ip.type = ESP_IPADDR_TYPE_V4,
    };
    esp_err_t ret = esp_netif_str_to_ip4(value, &dns.ip.u_addr.ip4);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_netif_set_dns_info(s_eth_netif, type, &dns);
}

static esp_err_t apply_config(const si_network_config_t *config)
{
    if (!s_eth_netif || !config ||
        !si_network_config_validate(config, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_netif_set_hostname(s_eth_netif, config->hostname);
    if (ret != ESP_OK) {
        return ret;
    }

    status_lock();
    s_status.connected = false;
    clear_ip_status_locked();
    status_unlock();

    if (config->mode == SI_NETWORK_MODE_DHCP) {
        esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
        ret = esp_netif_dhcpc_get_status(s_eth_netif, &dhcp_status);
        if (ret == ESP_OK && dhcp_status != ESP_NETIF_DHCP_STARTED) {
            ret = esp_netif_dhcpc_start(s_eth_netif);
            if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ret = ESP_OK;
            }
        }
        /*
         * Reapplying DHCP while the client is already running does not emit a
         * second got-IP event.  Preserve the live lease in status instead of
         * leaving the control plane falsely disconnected.
         */
        if (ret == ESP_OK) {
            esp_netif_ip_info_t ip_info = {0};
            if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK &&
                ip_info.ip.addr != 0) {
                status_lock();
                s_status.connected = s_status.link_up;
                refresh_ip_status_for_mode_locked(
                    &ip_info, SI_NETWORK_MODE_DHCP);
                status_unlock();
            }
        }
    } else {
        ret = stop_dhcp_if_needed();
        esp_netif_ip_info_t ip_info = {0};
        if (ret == ESP_OK) {
            ret = esp_netif_str_to_ip4(config->address, &ip_info.ip);
        }
        if (ret == ESP_OK) {
            ret = esp_netif_str_to_ip4(config->netmask, &ip_info.netmask);
        }
        if (ret == ESP_OK && config->gateway[0]) {
            ret = esp_netif_str_to_ip4(config->gateway, &ip_info.gw);
        }
        if (ret == ESP_OK) {
            ret = esp_netif_set_ip_info(s_eth_netif, &ip_info);
        }
        if (ret == ESP_OK) {
            ret = set_dns(ESP_NETIF_DNS_MAIN, config->dns_primary);
        }
        if (ret == ESP_OK) {
            ret = set_dns(ESP_NETIF_DNS_BACKUP, config->dns_secondary);
        }
        if (ret == ESP_OK) {
            status_lock();
            s_status.connected = s_status.link_up;
            refresh_ip_status_for_mode_locked(
                &ip_info, SI_NETWORK_MODE_STATIC);
            status_unlock();
        }
    }

    if (ret == ESP_OK) {
        s_runtime_config = *config;
        status_lock();
        s_status.recovery_active = false;
        update_config_status_locked(config,
                                    s_status.pending_confirmation
                                        ? "pending"
                                        : "active");
        status_unlock();
    }
    return ret;
}

static void finish_static_acd(static_acd_result_t result)
{
    if (!s_operation_lock || result == STATIC_ACD_IDLE ||
        result == STATIC_ACD_WAITING) {
        return;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    if (s_static_acd_result != result) {
        xSemaphoreGive(s_operation_lock);
        return;
    }
    stop_static_acd();

    si_network_settings_status_t settings = {0};
    esp_err_t ret = si_network_settings_get(&settings);
    if (ret == ESP_OK && !settings.have_staged) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && result == STATIC_ACD_OK) {
        ret = apply_config(&settings.staged);
    }
    if (ret == ESP_OK && result == STATIC_ACD_OK) {
        status_lock();
        update_config_status_locked(&settings.staged, "pending");
        status_unlock();
        ESP_LOGI(TAG, "Static IPv4 address conflict check passed: %s",
                 settings.staged.address);
    } else {
        esp_err_t rollback_ret = ESP_OK;
        if (ret == ESP_OK) {
            ret = result == STATIC_ACD_CONFLICT
                      ? ESP_ERR_INVALID_STATE
                      : ESP_ERR_TIMEOUT;
        }
        if (settings.have_staged) {
            rollback_ret = rollback_settings_on_internal_stack();
        }
        s_pending_deadline_us = 0;
        status_lock();
        s_status.pending_confirmation = false;
        s_status.confirm_remaining_seconds = 0;
        if (rollback_ret == ESP_OK) {
            update_config_status_locked(&settings.active, "active");
        }
        status_unlock();
        if (result == STATIC_ACD_CONFLICT) {
            ESP_LOGE(TAG, "Static IPv4 address is already in use: %s",
                     settings.staged.address);
        } else {
            ESP_LOGE(TAG, "Static IPv4 address conflict check timed out");
        }
    }
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret,
                   result == STATIC_ACD_CONFLICT
                       ? "static IPv4 address conflict"
                       : "static IPv4 conflict check");
}

/* Caller owns s_operation_lock. Keeping the decision and mutation under the
 * same lock prevents a stale timeout observation from rolling back a newer
 * staged generation or racing a just-completed confirmation. */
static esp_err_t rollback_pending_locked(void)
{
    stop_static_acd();
    si_network_settings_status_t settings;
    esp_err_t ret = si_network_settings_get(&settings);
    if (ret == ESP_OK) {
        ret = rollback_settings_on_internal_stack();
    }
    if (ret == ESP_OK) {
        s_pending_deadline_us = 0;
        status_lock();
        s_status.pending_confirmation = false;
        s_status.confirm_remaining_seconds = 0;
        update_config_status_locked(&settings.active, "active");
        status_unlock();
        ret = apply_config(&settings.active);
    }
    return ret;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    status_lock();
    switch (event_id) {
    case ETHERNET_EVENT_START:
        break;
    case ETHERNET_EVENT_CONNECTED:
        s_status.link_up = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_status.link_up = false;
        s_status.connected = false;
        s_status.speed_mbps = 0;
        s_status.full_duplex = false;
        clear_ip_status_locked();
        break;
    case ETHERNET_EVENT_STOP:
        s_status.link_up = false;
        s_status.connected = false;
        s_status.speed_mbps = 0;
        s_status.full_duplex = false;
        clear_ip_status_locked();
        break;
    default:
        break;
    }
    status_unlock();
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    const ip_event_got_ip_t *event =
        (const ip_event_got_ip_t *)event_data;
    status_lock();
    s_status.link_up = true;
    s_status.connected = true;
    s_status.retry_count = 0;
    refresh_ip_status_locked(&event->ip_info);
    status_unlock();
}

static void lost_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    status_lock();
    s_status.connected = false;
    clear_ip_status_locked();
    status_unlock();
}

static esp_err_t install_eth_driver_with_retry(
    const eth_esp32_emac_config_t *emac_config,
    const eth_mac_config_t *mac_config,
    const eth_phy_config_t *phy_config)
{
    esp_err_t last_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= ETH_DRIVER_INSTALL_ATTEMPTS; ++attempt) {
        esp_eth_mac_t *mac = esp_eth_mac_new_esp32(emac_config, mac_config);
        ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_NO_MEM, TAG,
                            "create ESP32-P4 EMAC");
#ifdef CONFIG_SI_ETH_PHY_DP83825
        esp_eth_phy_t *phy = esp_eth_phy_new_generic(phy_config);
#else
        esp_eth_phy_t *phy = esp_eth_phy_new_ip101(phy_config);
#endif
        if (!phy) {
            mac->del(mac);
            return ESP_ERR_NO_MEM;
        }
        esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
        s_eth_handle = NULL;
        last_ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
        if (last_ret == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG,
                         "Ethernet driver recovered on install attempt %d/%d",
                         attempt, ETH_DRIVER_INSTALL_ATTEMPTS);
            }
            status_lock();
            s_status.retry_count = attempt - 1;
            status_unlock();
            return ESP_OK;
        }
        s_eth_handle = NULL;
        phy->del(phy);
        mac->del(mac);
        status_lock();
        s_status.retry_count = attempt;
        status_unlock();
        ESP_LOGW(TAG, "Ethernet driver install attempt %d/%d failed: %s",
                 attempt, ETH_DRIVER_INSTALL_ATTEMPTS,
                 esp_err_to_name(last_ret));
        if (attempt < ETH_DRIVER_INSTALL_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(ETH_DRIVER_INSTALL_RETRY_DELAY_MS));
        }
    }
    return last_ret;
}

static void manager_task(void *arg)
{
    (void)arg;
    TickType_t last_refresh = 0;
    while (true) {
        TickType_t now_ticks = xTaskGetTickCount();
        if (now_ticks - last_refresh >=
            pdMS_TO_TICKS(NETWORK_LINK_REFRESH_MS)) {
            copy_eth_mac();
            refresh_link_properties();
            last_refresh = now_ticks;
        }

        bool timeout = false;
        status_lock();
        if (s_status.pending_confirmation && s_pending_deadline_us > 0) {
            int64_t remaining_us =
                s_pending_deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                timeout = true;
                s_status.confirm_remaining_seconds = 0;
            } else {
                s_status.confirm_remaining_seconds =
                    (uint32_t)((remaining_us + 999999) / 1000000);
            }
        }
        status_unlock();
        if (timeout) {
            xSemaphoreTake(s_operation_lock, portMAX_DELAY);
            bool still_expired = false;
            status_lock();
            if (s_status.pending_confirmation &&
                s_pending_deadline_us > 0 &&
                s_pending_deadline_us <= esp_timer_get_time()) {
                still_expired = true;
                /* Automatic expiry is one-shot. If persistence cannot allocate
                 * or NVS returns an error, keep pending state and expose the
                 * error for an explicit retry instead of spawning every 250 ms. */
                s_pending_deadline_us = 0;
                s_status.confirm_remaining_seconds = 0;
            }
            status_unlock();
            esp_err_t ret = ESP_OK;
            if (still_expired) {
                ESP_LOGW(TAG,
                         "Pending network configuration timed out; rolling back");
                ret = rollback_pending_locked();
            }
            xSemaphoreGive(s_operation_lock);
            if (still_expired) {
                set_last_error(ret, "automatic rollback");
            }
        }

        static_acd_result_t acd_result = s_static_acd_result;
        if (acd_result == STATIC_ACD_WAITING &&
            s_static_acd_deadline_us > 0 &&
            esp_timer_get_time() >= s_static_acd_deadline_us) {
            s_static_acd_result = STATIC_ACD_TIMEOUT;
            acd_result = STATIC_ACD_TIMEOUT;
        }
        if (acd_result == STATIC_ACD_OK ||
            acd_result == STATIC_ACD_CONFLICT ||
            acd_result == STATIC_ACD_TIMEOUT) {
            finish_static_acd(acd_result);
        }
        vTaskDelay(pdMS_TO_TICKS(NETWORK_MANAGER_INTERVAL_MS));
    }
}

esp_err_t si_net_init(void)
{
    if (!s_status_lock) {
        s_status_lock = xSemaphoreCreateMutex();
    }
    if (!s_operation_lock) {
        s_operation_lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_status_lock && s_operation_lock,
                        ESP_ERR_NO_MEM, TAG, "create network mutex");
    memset(&s_status, 0, sizeof(s_status));
    s_status.configured = SI_CFG_ETH_ENABLED;
    strlcpy(s_status.interface, "ethernet", sizeof(s_status.interface));
    strlcpy(s_status.driver, SI_CFG_ETH_DRIVER_NAME, sizeof(s_status.driver));

#if !SI_CFG_ETH_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint8_t base_mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(base_mac, ESP_MAC_BASE),
                        TAG, "read base MAC");
    si_product_identity_default_from_mac(base_mac, &s_identity);
    strlcpy(s_status.device_id, s_identity.device_id,
            sizeof(s_status.device_id));
    si_network_config_default(&s_factory_config, s_identity.hostname);

    si_network_settings_status_t settings;
    ESP_RETURN_ON_ERROR(si_network_settings_initialize(
                            &s_factory_config, &settings),
                        TAG, "initialize network settings");
    s_runtime_config = settings.active;
    s_status.recovered_pending = settings.recovered_pending;
    update_config_status_locked(&settings.active, "active");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        return event_ret;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = SI_CFG_ETH_PHY_ADDRESS;
    phy_config.reset_gpio_num = SI_CFG_ETH_PHY_RESET_GPIO;
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = SI_CFG_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = SI_CFG_ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = SI_CFG_ETH_RMII_CLOCK_GPIO;
    emac_config.emac_dataif_gpio.rmii.tx_en_num =
        SI_CFG_ETH_RMII_TX_ENABLE_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = SI_CFG_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = SI_CFG_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num =
        SI_CFG_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = SI_CFG_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = SI_CFG_ETH_RMII_RXD1_GPIO;

    ESP_RETURN_ON_ERROR(install_eth_driver_with_retry(
                            &emac_config, &mac_config, &phy_config),
                        TAG, "install Ethernet driver");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_ERR_NO_MEM, TAG,
                        "create Ethernet netif");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue, ESP_ERR_NO_MEM, TAG,
                        "create Ethernet netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue),
                        TAG, "attach Ethernet netif");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            ETH_EVENT, ESP_EVENT_ANY_ID,
                            &eth_event_handler, NULL),
                        TAG, "register Ethernet event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_ETH_GOT_IP,
                            &got_ip_event_handler, NULL),
                        TAG, "register Ethernet IP handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_ETH_LOST_IP,
                            &lost_ip_event_handler, NULL),
                        TAG, "register Ethernet lost IP handler");

    esp_err_t stored_config_ret = apply_config(&settings.active);
    if (stored_config_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Stored network configuration failed (%s); starting "
                 "factory DHCP recovery",
                 esp_err_to_name(stored_config_ret));
        esp_err_t recovery_ret = apply_config(&s_factory_config);
        if (recovery_ret != ESP_OK) {
            set_last_error(recovery_ret, "factory DHCP recovery");
            return recovery_ret;
        }
        status_lock();
        s_status.recovery_active = true;
        update_config_status_locked(&s_factory_config, "recovery");
        status_unlock();
        set_last_error(stored_config_ret, "stored config rejected");
    }
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "start Ethernet");
    copy_eth_mac();
    status_lock();
    s_status.initialized = true;
    status_unlock();
    if (!s_manager_task) {
        BaseType_t created;
#if SI_CFG_VIDEO_H264_ENABLED
        created = xTaskCreateWithCaps(
            manager_task, "si_net_mgr", 4096, NULL, 5, &s_manager_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        created = xTaskCreate(manager_task, "si_net_mgr", 4096, NULL, 5,
                              &s_manager_task);
#endif
        ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                            "create network manager task");
    }
    ESP_LOGI(TAG,
             "Network ready without waiting for lease: device=%s hostname=%s "
             "mode=%s autoip=%d recovery=%d",
             s_identity.device_id, s_runtime_config.hostname,
             si_network_mode_name(s_runtime_config.mode),
             s_runtime_config.autoip_fallback,
             stored_config_ret != ESP_OK);
    return ESP_OK;
#endif
}

void si_net_get_status(si_net_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    status_lock();
    *out = s_status;
    status_unlock();
}

esp_err_t si_net_get_active_config(si_network_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    si_network_settings_status_t settings;
    esp_err_t ret = si_network_settings_get(&settings);
    if (ret == ESP_OK) {
        *out = settings.active;
    }
    return ret;
}

esp_err_t si_net_get_staged_config(si_network_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    si_network_settings_status_t settings;
    esp_err_t ret = si_network_settings_get(&settings);
    if (ret == ESP_OK && !settings.have_staged) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        *out = settings.staged;
    }
    return ret;
}

esp_err_t si_net_stage_config(const si_network_config_t *config)
{
    if (!s_operation_lock || !config) {
        return ESP_ERR_INVALID_STATE;
    }
    char validation_error[SI_NETWORK_VALIDATION_ERROR_MAX_LEN];
    if (!si_network_config_validate(
            config, validation_error, sizeof(validation_error))) {
        status_lock();
        strlcpy(s_status.last_error, validation_error,
                sizeof(s_status.last_error));
        status_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    esp_err_t ret = si_network_settings_stage(config);
    if (ret == ESP_OK) {
        status_lock();
        strlcpy(s_status.config_state, "staged",
                sizeof(s_status.config_state));
        status_unlock();
    }
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret, "stage config");
    return ret;
}

esp_err_t si_net_apply_staged(void)
{
    if (!s_operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    si_network_settings_status_t settings;
    esp_err_t ret = si_network_settings_get(&settings);
    bool have_settings = ret == ESP_OK;
    if (ret == ESP_OK && !settings.have_staged) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = si_network_settings_mark_pending();
    }
    bool run_static_acd = false;
    if (ret == ESP_OK) {
        status_lock();
        run_static_acd = settings.staged.mode == SI_NETWORK_MODE_STATIC &&
                         s_status.link_up;
        status_unlock();
    }
    if (ret == ESP_OK) {
        status_lock();
        s_status.pending_confirmation = true;
        s_status.confirm_remaining_seconds =
            NETWORK_CONFIRM_TIMEOUT_SECONDS;
        update_config_status_locked(&settings.staged,
                                    run_static_acd ? "checking" : "pending");
        status_unlock();
        s_pending_deadline_us =
            esp_timer_get_time() +
            (int64_t)NETWORK_CONFIRM_TIMEOUT_SECONDS * 1000000LL;
        ret = run_static_acd ? start_static_acd(&settings.staged)
                             : apply_config(&settings.staged);
    }
    if (ret != ESP_OK && have_settings) {
        si_network_settings_rollback();
        s_pending_deadline_us = 0;
        status_lock();
        s_status.pending_confirmation = false;
        s_status.confirm_remaining_seconds = 0;
        update_config_status_locked(&settings.active, "active");
        status_unlock();
        apply_config(&settings.active);
    }
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret, "apply config");
    return ret;
}

esp_err_t si_net_commit_pending(void)
{
    if (!s_operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    esp_err_t ret = s_static_acd_result == STATIC_ACD_WAITING
                        ? ESP_ERR_INVALID_STATE
                        : si_network_settings_confirm();
    if (ret == ESP_OK) {
        s_pending_deadline_us = 0;
        status_lock();
        s_status.pending_confirmation = false;
        s_status.confirm_remaining_seconds = 0;
        update_config_status_locked(&s_runtime_config, "active");
        status_unlock();
    }
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret, "commit config");
    return ret;
}

esp_err_t si_net_rollback_pending(void)
{
    if (!s_operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    esp_err_t ret = rollback_pending_locked();
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret, "rollback config");
    return ret;
}

esp_err_t si_net_reset_config(void)
{
    if (!s_operation_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    stop_static_acd();
    esp_err_t ret = si_network_settings_reset(&s_factory_config);
    if (ret == ESP_OK) {
        s_pending_deadline_us = 0;
        status_lock();
        s_status.pending_confirmation = false;
        s_status.confirm_remaining_seconds = 0;
        update_config_status_locked(&s_factory_config, "active");
        status_unlock();
        ret = apply_config(&s_factory_config);
    }
    xSemaphoreGive(s_operation_lock);
    set_last_error(ret, "reset config");
    return ret;
}
