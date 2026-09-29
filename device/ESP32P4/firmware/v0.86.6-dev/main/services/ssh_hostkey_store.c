#include "ssh_hostkey_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool si_ssh_hostkey_record_valid(const char *fingerprint_hex)
{
    if (!fingerprint_hex ||
        strlen(fingerprint_hex) != SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN; i++) {
        const char c = fingerprint_hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool si_ssh_hostkey_record_matches(const char *saved,
                                   const char *observed)
{
    if (!si_ssh_hostkey_record_valid(saved) ||
        !si_ssh_hostkey_record_valid(observed)) {
        return false;
    }

    uint8_t diff = 0;
    for (size_t i = 0; i < SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN; i++) {
        diff |= (uint8_t)(saved[i] ^ observed[i]);
    }
    return diff == 0;
}

#ifdef ESP_PLATFORM

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings_store.h"

#define SI_SSH_HOSTKEY_NAMESPACE "si_ssh_tofu"
#define SI_SSH_HOSTKEY_NVS_KEY_MAX_LEN 15
#define SI_SSH_HOSTKEY_BROKER_STACK_SIZE 4096
#define SI_SSH_HOSTKEY_BROKER_WAIT_MS 5000U
#define SI_SSH_HOSTKEY_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    char key[SI_SSH_HOSTKEY_NVS_KEY_MAX_LEN + 1];
    char observed[SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN + 1];
    SemaphoreHandle_t done;
    volatile uint32_t references;
    esp_err_t result;
    bool trusted_new;
} ssh_hostkey_request_t;

/* Keep a timed-out broker from multiplying internal stacks or NVS writers. */
static bool s_ssh_hostkey_broker_active;

static void ssh_hostkey_secure_clear(void *data, size_t len)
{
    static void *(*const volatile memset_fn)(void *, int, size_t) = memset;
    if (data && len > 0) {
        (void)memset_fn(data, 0, len);
    }
}

static void ssh_hostkey_request_release(ssh_hostkey_request_t *request)
{
    if (!request ||
        __atomic_sub_fetch(&request->references, 1U, __ATOMIC_ACQ_REL) != 0U) {
        return;
    }
    SemaphoreHandle_t done = request->done;
    ssh_hostkey_secure_clear(request, sizeof(*request));
    if (done) {
        vSemaphoreDeleteWithCaps(done);
    }
    heap_caps_free(request);
}

static esp_err_t ssh_hostkey_store_transaction(ssh_hostkey_request_t *request)
{
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    char saved[SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN + 1] = {0};

    esp_err_t ret = si_settings_store_open_read(
        &store, SI_SSH_HOSTKEY_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_settings_store_get_string(
            &store, request->key, saved, sizeof(saved));
        si_settings_store_close(&store);
        if (ret == ESP_OK) {
            bool matches = si_ssh_hostkey_record_matches(
                saved, request->observed);
            ssh_hostkey_secure_clear(saved, sizeof(saved));
            return matches ? ESP_OK : ESP_ERR_INVALID_CRC;
        }
        ssh_hostkey_secure_clear(saved, sizeof(saved));
        if (ret != ESP_ERR_NOT_FOUND) {
            return ret;
        }
    } else if (ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    /* The SSH service serializes sessions, so this first-write is single-owner. */
    ret = si_settings_store_open_write(&store, SI_SSH_HOSTKEY_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_string(
            &store, request->key, request->observed);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        request->trusted_new = true;
    }
    return ret;
}

static void ssh_hostkey_broker_task(void *arg)
{
    ssh_hostkey_request_t *request = (ssh_hostkey_request_t *)arg;
    volatile uint8_t stack_probe = 0;
    if (!esp_ptr_in_dram((const void *)&stack_probe) ||
        !esp_ptr_in_dram((const void *)request)) {
        request->result = ESP_ERR_INVALID_STATE;
    } else {
        request->result = ssh_hostkey_store_transaction(request);
    }
    __atomic_store_n(&s_ssh_hostkey_broker_active, false, __ATOMIC_RELEASE);
    xSemaphoreGive(request->done);
    ssh_hostkey_request_release(request);
    /* Ordinary FreeRTOS tasks use the internal heap and self-delete without
     * IDF's WithCaps cleanup task, whose low-memory failure path aborts. */
    vTaskDelete(NULL);
}

esp_err_t si_ssh_hostkey_verify_or_trust(const char *key,
                                         const char *fingerprint_hex,
                                         bool *trusted_new_out)
{
    if (trusted_new_out) {
        *trusted_new_out = false;
    }
    if (!key || key[0] == '\0' ||
        strnlen(key, SI_SSH_HOSTKEY_NVS_KEY_MAX_LEN + 1) >
            SI_SSH_HOSTKEY_NVS_KEY_MAX_LEN ||
        !si_ssh_hostkey_record_valid(fingerprint_hex)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool expected_inactive = false;
    if (!__atomic_compare_exchange_n(&s_ssh_hostkey_broker_active,
                                     &expected_inactive, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }

    ssh_hostkey_request_t *request = heap_caps_calloc(
        1, sizeof(*request), SI_SSH_HOSTKEY_INTERNAL_CAPS);
    if (!request) {
        __atomic_store_n(&s_ssh_hostkey_broker_active, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(request->key, key, sizeof(request->key));
    strlcpy(request->observed, fingerprint_hex, sizeof(request->observed));
    request->done = xSemaphoreCreateBinaryWithCaps(SI_SSH_HOSTKEY_INTERNAL_CAPS);
    request->references = 2U; /* caller plus broker task */
    request->result = ESP_FAIL;
    if (!request->done) {
        __atomic_store_n(&s_ssh_hostkey_broker_active, false, __ATOMIC_RELEASE);
        ssh_hostkey_request_release(request);
        ssh_hostkey_request_release(request);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        ssh_hostkey_broker_task, "si_ssh_tofu", SI_SSH_HOSTKEY_BROKER_STACK_SIZE,
        request, tskIDLE_PRIORITY + 3, NULL, tskNO_AFFINITY);
    if (task_ok != pdPASS) {
        __atomic_store_n(&s_ssh_hostkey_broker_active, false, __ATOMIC_RELEASE);
        ssh_hostkey_request_release(request);
        ssh_hostkey_request_release(request);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(request->done,
                       pdMS_TO_TICKS(SI_SSH_HOSTKEY_BROKER_WAIT_MS)) == pdTRUE) {
        ret = request->result;
        if (ret == ESP_OK && trusted_new_out) {
            *trusted_new_out = request->trusted_new;
        }
    }
    ssh_hostkey_request_release(request);
    return ret;
}

#endif
