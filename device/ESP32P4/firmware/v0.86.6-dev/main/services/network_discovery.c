#include "network_discovery.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "discovery_rate_limit.h"
#include "device_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net_manager.h"

static const char *TAG = "si-discovery";

#define DISCOVERY_TASK_STACK 5120
#define DISCOVERY_REQUEST_MAX 256
#define DISCOVERY_RESPONSE_MAX 512
#define DISCOVERY_NONCE_MAX 64
#define DISCOVERY_DISABLED_POLL_MS 250

static TaskHandle_t s_discovery_task;
static si_discovery_rate_limiter_t s_rate_limiter;

static int open_discovery_socket(void)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "create UDP socket failed errno=%d", errno);
        return -1;
    }
    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
               sizeof(timeout));
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(SI_NETWORK_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "bind UDP/%u failed errno=%d",
                 SI_NETWORK_DISCOVERY_PORT, errno);
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static esp_err_t publish_mdns_service(void)
{
    return mdns_service_add(NULL, "_exoanchor", "_tcp",
                            SI_DEFAULT_HTTP_PORT, NULL, 0);
}

static void unpublish_mdns_service(void)
{
    esp_err_t ret = mdns_service_remove("_exoanchor", "_tcp");
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND &&
        ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "remove mDNS discovery service returned %s",
                 esp_err_to_name(ret));
    }
}

static bool parse_discovery_request(char *data, char *nonce,
                                    size_t nonce_size)
{
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        return false;
    }
    const cJSON *service =
        cJSON_GetObjectItemCaseSensitive(root, "service");
    const cJSON *version =
        cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *nonce_json =
        cJSON_GetObjectItemCaseSensitive(root, "nonce");
    bool valid = cJSON_IsString(service) &&
                 strcmp(service->valuestring, "exoanchor.discover") == 0 &&
                 cJSON_IsNumber(version) && version->valueint == 1 &&
                 cJSON_IsString(nonce_json) &&
                 nonce_json->valuestring[0] &&
                 strlen(nonce_json->valuestring) <= DISCOVERY_NONCE_MAX;
    if (valid) {
        strlcpy(nonce, nonce_json->valuestring, nonce_size);
    }
    cJSON_Delete(root);
    return valid;
}

static char *build_discovery_response(const char *nonce)
{
    si_net_status_t status;
    si_net_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "service", "exoanchor");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "nonce", nonce);
    cJSON_AddStringToObject(root, "device_id", status.device_id);
    cJSON_AddStringToObject(root, "hostname", status.hostname);
    cJSON_AddStringToObject(root, "ipv4", status.ip);
    cJSON_AddStringToObject(root, "interface", status.interface);
    cJSON_AddStringToObject(root, "address_source",
                            status.address_source);
    cJSON_AddStringToObject(root, "firmware", SI_BMC_VERSION);
    cJSON_AddStringToObject(root, "board", SI_BOARD_ID);
    cJSON_AddNumberToObject(root, "http_port", SI_DEFAULT_HTTP_PORT);
    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return response;
}

static void sync_mdns_identity(char *last_hostname,
                               size_t last_hostname_size,
                               char *last_source,
                               size_t last_source_size)
{
    si_net_status_t status;
    si_net_get_status(&status);
    if (!status.hostname[0]) {
        return;
    }
    bool hostname_changed =
        strcmp(last_hostname, status.hostname) != 0;
    const char *source = status.address_source[0]
                             ? status.address_source
                             : "pending";
    bool source_changed = strcmp(last_source, source) != 0;
    if (!hostname_changed && !source_changed) {
        return;
    }
    if (hostname_changed &&
        mdns_hostname_set(status.hostname) == ESP_OK) {
        strlcpy(last_hostname, status.hostname, last_hostname_size);
    }
    mdns_txt_item_t txt[] = {
        {"id", status.device_id},
        {"fw", SI_BMC_VERSION},
        {"board", SI_BOARD_ID},
        {"source", source},
    };
    if (mdns_service_txt_set("_exoanchor", "_tcp", txt,
                             sizeof(txt) / sizeof(txt[0])) == ESP_OK) {
        strlcpy(last_source, source, last_source_size);
    }
}

static void discovery_task(void *arg)
{
    (void)arg;
    int socket_fd = -1;
    bool mdns_published = false;
    char last_hostname[SI_PRODUCT_HOSTNAME_MAX_LEN + 1] = {0};
    char last_source[sizeof(((si_net_status_t *)0)->address_source)] = {0};
    char request[DISCOVERY_REQUEST_MAX + 2];
    while (true) {
        if (!si_lan_discovery_is_enabled_cached()) {
            if (socket_fd >= 0) {
                close(socket_fd);
                socket_fd = -1;
            }
            if (mdns_published) {
                unpublish_mdns_service();
                mdns_published = false;
                last_hostname[0] = '\0';
                last_source[0] = '\0';
                ESP_LOGI(TAG, "LAN discovery disabled");
            }
            vTaskDelay(pdMS_TO_TICKS(DISCOVERY_DISABLED_POLL_MS));
            continue;
        }
        if (socket_fd < 0) {
            socket_fd = open_discovery_socket();
            if (socket_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (!mdns_published) {
            esp_err_t ret = publish_mdns_service();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "publish mDNS discovery service returned %s",
                         esp_err_to_name(ret));
                close(socket_fd);
                socket_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            mdns_published = true;
            ESP_LOGI(TAG,
                     "Discovery ready: _exoanchor._tcp.local and UDP/%u",
                     SI_NETWORK_DISCOVERY_PORT);
        }
        sync_mdns_identity(last_hostname, sizeof(last_hostname),
                           last_source, sizeof(last_source));
        struct sockaddr_storage source;
        socklen_t source_len = sizeof(source);
        int received = recvfrom(socket_fd, request,
                                DISCOVERY_REQUEST_MAX + 1, 0,
                                (struct sockaddr *)&source, &source_len);
        if (received <= 0) {
            continue;
        }
        if (received > DISCOVERY_REQUEST_MAX ||
            source.ss_family != AF_INET) {
            continue;
        }
        if (!si_lan_discovery_is_enabled_cached()) {
            continue;
        }
        const struct sockaddr_in *source_ipv4 =
            (const struct sockaddr_in *)&source;
        if (!si_discovery_rate_limit_allow(
                &s_rate_limiter, source_ipv4->sin_addr.s_addr,
                (uint32_t)(esp_timer_get_time() / 1000))) {
            continue;
        }
        request[received] = '\0';
        char nonce[DISCOVERY_NONCE_MAX + 1] = {0};
        if (!parse_discovery_request(request, nonce, sizeof(nonce))) {
            continue;
        }
        char *response = build_discovery_response(nonce);
        if (response) {
            size_t response_len = strlen(response);
            if (response_len <= DISCOVERY_RESPONSE_MAX) {
                sendto(socket_fd, response, response_len, 0,
                       (struct sockaddr *)&source, source_len);
            } else {
                ESP_LOGW(TAG, "drop oversized discovery response: %u bytes",
                         (unsigned)response_len);
            }
            free(response);
        }
    }
}

esp_err_t si_network_discovery_start(void)
{
    if (s_discovery_task) {
        return ESP_OK;
    }
    /*
     * H.264 builds deliberately place this task stack in PSRAM to preserve
     * the largest internal block for the Agent worker.  Load NVS here, while
     * still running on app_main's internal stack; the PSRAM task must never
     * become the caller that starts a cache-disabling flash operation.
     */
    esp_err_t feature_ret = si_product_feature_settings_preload();
    if (feature_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Product feature retry failed; discovery remains disabled: %s",
                 esp_err_to_name(feature_ret));
    }
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    si_net_status_t status;
    si_net_get_status(&status);
    if (status.hostname[0]) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(status.hostname));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        mdns_instance_name_set("ExoAnchor ESP32-P4"));
    BaseType_t created;
#if SI_CFG_VIDEO_H264_ENABLED
    created = xTaskCreateWithCaps(
        discovery_task, "si_discovery", DISCOVERY_TASK_STACK,
        NULL, 4, &s_discovery_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    created = xTaskCreate(discovery_task, "si_discovery",
                          DISCOVERY_TASK_STACK, NULL, 4,
                          &s_discovery_task);
#endif
    if (created != pdPASS) {
        s_discovery_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LAN discovery controller started");
    return ESP_OK;
}
