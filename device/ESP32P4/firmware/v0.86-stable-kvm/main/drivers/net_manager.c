// Ethernet hardware driver and network lifecycle.
#include "net_manager.h"

#include <inttypes.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"

static const char *TAG = "si-net";

#define ETH_GOT_IP_BIT BIT0

static EventGroupHandle_t s_net_event_group;
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;
static si_net_status_t s_status;

static void copy_ip(char *dst, size_t dst_size, const esp_ip4_addr_t *addr)
{
    snprintf(dst, dst_size, IPSTR, IP2STR(addr));
}

static void clear_ip_status(void)
{
    s_status.ip[0] = '\0';
    s_status.netmask[0] = '\0';
    s_status.gateway[0] = '\0';
}

static void refresh_ip_status(const esp_netif_ip_info_t *ip_info)
{
    if (!ip_info) {
        return;
    }
    copy_ip(s_status.ip, sizeof(s_status.ip), &ip_info->ip);
    copy_ip(s_status.netmask, sizeof(s_status.netmask), &ip_info->netmask);
    copy_ip(s_status.gateway, sizeof(s_status.gateway), &ip_info->gw);
}

static void copy_eth_mac(void)
{
    uint8_t mac[6] = {0};
    if (s_eth_handle && esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
        snprintf(s_status.mac, sizeof(s_status.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static void refresh_link_status(void)
{
    if (!s_eth_handle || !s_status.link_up) {
        s_status.speed_mbps = 0;
        s_status.full_duplex = false;
        return;
    }

    s_status.speed_mbps = 0;
    s_status.full_duplex = false;
    eth_speed_t speed = ETH_SPEED_10M;
    esp_err_t speed_ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed);
    if (speed_ret == ESP_OK) {
        s_status.speed_mbps = speed == ETH_SPEED_100M ? 100 : 10;
    } else {
        ESP_LOGW(TAG, "read Ethernet link speed failed: %s", esp_err_to_name(speed_ret));
    }

    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    esp_err_t duplex_ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
    if (duplex_ret == ESP_OK) {
        s_status.full_duplex = duplex == ETH_DUPLEX_FULL;
    } else {
        ESP_LOGW(TAG, "read Ethernet duplex mode failed: %s", esp_err_to_name(duplex_ret));
    }
}

static void log_link_status(const char *prefix)
{
    ESP_LOGI(TAG, "%s: %dMbps %s-duplex MAC=%s",
             prefix,
             s_status.speed_mbps,
             s_status.full_duplex ? "full" : "half",
             s_status.mac[0] ? s_status.mac : "unknown");
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_CONNECTED:
        s_status.link_up = true;
        copy_eth_mac();
        refresh_link_status();
        log_link_status("Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_status.link_up = false;
        s_status.connected = false;
        refresh_link_status();
        clear_ip_status();
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_STOP:
        s_status.link_up = false;
        s_status.connected = false;
        refresh_link_status();
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_status.link_up = true;
    s_status.connected = true;
    s_status.retry_count = 0;
    refresh_ip_status(&event->ip_info);
    refresh_link_status();
    ESP_LOGI(TAG, "Ethernet got IP: %s link=%dMbps %s-duplex",
             s_status.ip,
             s_status.speed_mbps,
             s_status.full_duplex ? "full" : "half");
    ESP_LOGI(TAG, "ETHMASK:%s", s_status.netmask);
    ESP_LOGI(TAG, "ETHGW:%s", s_status.gateway);
    xEventGroupSetBits(s_net_event_group, ETH_GOT_IP_BIT);
}

esp_err_t si_net_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.configured = SI_CFG_ETH_ENABLED;
    strlcpy(s_status.interface, "ethernet", sizeof(s_status.interface));
    strlcpy(s_status.driver, SI_CFG_ETH_DRIVER_NAME, sizeof(s_status.driver));
    strlcpy(s_status.phy, SI_CFG_ETH_PHY_NAME, sizeof(s_status.phy));

#if !SI_CFG_ETH_ENABLED
    ESP_LOGW(TAG, "Ethernet disabled by CONFIG_SI_ETH_ENABLE");
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(event_ret, TAG, "esp_event_loop_create_default");
    }

    s_net_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_net_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create event group");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = SI_CFG_ETH_PHY_ADDRESS;
    phy_config.reset_gpio_num = SI_CFG_ETH_PHY_RESET_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = SI_CFG_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = SI_CFG_ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = SI_CFG_ETH_RMII_CLOCK_GPIO;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = SI_CFG_ETH_RMII_TX_ENABLE_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = SI_CFG_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = SI_CFG_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = SI_CFG_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = SI_CFG_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = SI_CFG_ETH_RMII_RXD1_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_NO_MEM, TAG, "create ESP32-P4 EMAC");

#ifdef CONFIG_SI_ETH_PHY_DP83825
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
#else
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#endif
    ESP_RETURN_ON_FALSE(phy != NULL, ESP_ERR_NO_MEM, TAG, "create %s PHY",
                        SI_CFG_ETH_PHY_NAME);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle),
                        TAG, "install Ethernet driver");
    copy_eth_mac();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_ERR_NO_MEM, TAG, "create Ethernet netif");

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue != NULL, ESP_ERR_NO_MEM, TAG, "create Ethernet netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "attach Ethernet netif");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   &eth_event_handler, NULL),
                        TAG, "register Ethernet event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   &got_ip_event_handler, NULL),
                        TAG, "register Ethernet IP handler");

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "start Ethernet");
    ESP_LOGI(TAG,
             "Ethernet init: board=%s phy=%s driver=%s addr=%d reset=%d mdc=%d mdio=%d "
             "clk=%d tx_en=%d txd0=%d txd1=%d crs_dv=%d rxd0=%d rxd1=%d",
             SI_BOARD_ID, SI_CFG_ETH_PHY_NAME, SI_CFG_ETH_DRIVER_NAME,
             SI_CFG_ETH_PHY_ADDRESS, SI_CFG_ETH_PHY_RESET_GPIO,
             SI_CFG_ETH_MDC_GPIO, SI_CFG_ETH_MDIO_GPIO,
             SI_CFG_ETH_RMII_CLOCK_GPIO, SI_CFG_ETH_RMII_TX_ENABLE_GPIO,
             SI_CFG_ETH_RMII_TXD0_GPIO, SI_CFG_ETH_RMII_TXD1_GPIO,
             SI_CFG_ETH_RMII_CRS_DV_GPIO, SI_CFG_ETH_RMII_RXD0_GPIO,
             SI_CFG_ETH_RMII_RXD1_GPIO);

    EventBits_t bits = xEventGroupWaitBits(s_net_event_group,
                                           ETH_GOT_IP_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(SI_CFG_ETH_WAIT_IP_TIMEOUT_MS));
    if (bits & ETH_GOT_IP_BIT) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Ethernet DHCP still pending after %" PRIu32 " ms",
             (uint32_t)SI_CFG_ETH_WAIT_IP_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
#endif
}

void si_net_get_status(si_net_status_t *out)
{
    if (!out) {
        return;
    }

    s_status.configured = SI_CFG_ETH_ENABLED;
    refresh_link_status();
    if (s_status.connected && s_eth_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK) {
            refresh_ip_status(&ip_info);
        }
    }

    *out = s_status;
}
