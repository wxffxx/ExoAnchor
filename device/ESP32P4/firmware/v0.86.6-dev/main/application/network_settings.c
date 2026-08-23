#include "network_settings.h"

#include <string.h>

#include "settings_store.h"

#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#endif

#define NETWORK_SETTINGS_NAMESPACE "si_net_cfg"
#define NETWORK_SLOT_A 1U
#define NETWORK_SLOT_B 2U

#define KEY_ACTIVE "active"
#define KEY_STAGED "staged"
#define KEY_PENDING "pending"

/*
 * NVS disables the flash cache while serving reads.  Several consumers of
 * this module (notably the UART diagnostics task) deliberately keep their
 * stacks in PSRAM, so a seemingly read-only status query must not enter NVS.
 * Initialization and successful persistence operations publish this small
 * DRAM snapshot; readers only copy it while holding a short critical section.
 */
static si_network_settings_status_t s_cached_status;
static bool s_cached_status_valid;
#ifdef ESP_PLATFORM
static portMUX_TYPE s_cached_status_mux = portMUX_INITIALIZER_UNLOCKED;
#define CACHE_LOCK() portENTER_CRITICAL(&s_cached_status_mux)
#define CACHE_UNLOCK() portEXIT_CRITICAL(&s_cached_status_mux)
#else
#define CACHE_LOCK() ((void)0)
#define CACHE_UNLOCK() ((void)0)
#endif

static void invalidate_cached_status(void)
{
    CACHE_LOCK();
    memset(&s_cached_status, 0, sizeof(s_cached_status));
    s_cached_status_valid = false;
    CACHE_UNLOCK();
}

static void publish_cached_status(
    const si_network_settings_status_t *status)
{
    if (!status) {
        return;
    }
    CACHE_LOCK();
    s_cached_status = *status;
    s_cached_status_valid = true;
    CACHE_UNLOCK();
}

static esp_err_t copy_cached_status(
    si_network_settings_status_t *status_out)
{
    if (!status_out) {
        return ESP_ERR_INVALID_ARG;
    }
    CACHE_LOCK();
    bool valid = s_cached_status_valid;
    if (valid) {
        *status_out = s_cached_status;
    }
    CACHE_UNLOCK();
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool current_task_can_access_flash(void)
{
#ifdef ESP_PLATFORM
    /* Match the invariant asserted by esp_cache_freeze_caches_disable_interrupts. */
    volatile uint8_t stack_probe = 0;
    return esp_ptr_in_dram((const void *)&stack_probe)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast((const void *)&stack_probe)
#endif
        ;
#else
    return true;
#endif
}

#define REQUIRE_FLASH_SAFE_STACK()                                             \
    do {                                                                       \
        if (!current_task_can_access_flash()) {                                \
            return ESP_ERR_INVALID_STATE;                                      \
        }                                                                      \
    } while (0)

typedef struct {
    const char *schema;
    const char *generation;
    const char *mode;
    const char *autoip;
    const char *hostname;
    const char *address;
    const char *netmask;
    const char *gateway;
    const char *dns_primary;
    const char *dns_secondary;
} slot_keys_t;

static const slot_keys_t SLOT_A_KEYS = {
    .schema = "a_schema",
    .generation = "a_gen",
    .mode = "a_mode",
    .autoip = "a_auto",
    .hostname = "a_host",
    .address = "a_ip",
    .netmask = "a_mask",
    .gateway = "a_gw",
    .dns_primary = "a_dns1",
    .dns_secondary = "a_dns2",
};

static const slot_keys_t SLOT_B_KEYS = {
    .schema = "b_schema",
    .generation = "b_gen",
    .mode = "b_mode",
    .autoip = "b_auto",
    .hostname = "b_host",
    .address = "b_ip",
    .netmask = "b_mask",
    .gateway = "b_gw",
    .dns_primary = "b_dns1",
    .dns_secondary = "b_dns2",
};

static bool slot_valid(uint8_t slot)
{
    return slot == NETWORK_SLOT_A || slot == NETWORK_SLOT_B;
}

static uint8_t other_slot(uint8_t slot)
{
    return slot == NETWORK_SLOT_A ? NETWORK_SLOT_B : NETWORK_SLOT_A;
}

static const slot_keys_t *keys_for_slot(uint8_t slot)
{
    if (slot == NETWORK_SLOT_A) {
        return &SLOT_A_KEYS;
    }
    if (slot == NETWORK_SLOT_B) {
        return &SLOT_B_KEYS;
    }
    return NULL;
}

static esp_err_t read_optional_string(si_settings_store_t *store,
                                      const char *key, char *out,
                                      size_t out_size)
{
    esp_err_t ret = si_settings_store_get_string(store, key, out, out_size);
    if (ret == ESP_ERR_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

static esp_err_t read_slot(si_settings_store_t *store, uint8_t slot,
                           si_network_config_t *out)
{
    if (!store || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    const slot_keys_t *keys = keys_for_slot(slot);
    if (!keys) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    uint8_t mode = 0;
    uint8_t autoip = 0;
    esp_err_t ret = si_settings_store_get_u32(
        store, keys->schema, &out->schema_version);
    if (ret == ESP_OK) {
        ret = si_settings_store_get_u32(
            store, keys->generation, &out->generation);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_get_u8(store, keys->mode, &mode);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_get_u8(store, keys->autoip, &autoip);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_get_string(
            store, keys->hostname, out->hostname, sizeof(out->hostname));
    }
    if (ret == ESP_OK) {
        ret = read_optional_string(
            store, keys->address, out->address, sizeof(out->address));
    }
    if (ret == ESP_OK) {
        ret = read_optional_string(
            store, keys->netmask, out->netmask, sizeof(out->netmask));
    }
    if (ret == ESP_OK) {
        ret = read_optional_string(
            store, keys->gateway, out->gateway, sizeof(out->gateway));
    }
    if (ret == ESP_OK) {
        ret = read_optional_string(store, keys->dns_primary,
                                   out->dns_primary,
                                   sizeof(out->dns_primary));
    }
    if (ret == ESP_OK) {
        ret = read_optional_string(store, keys->dns_secondary,
                                   out->dns_secondary,
                                   sizeof(out->dns_secondary));
    }
    if (ret != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return ret;
    }
    out->mode = (si_network_mode_t)mode;
    out->autoip_fallback = autoip != 0;
    return si_network_config_validate(out, NULL, 0)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

static esp_err_t write_slot(si_settings_store_t *store, uint8_t slot,
                            const si_network_config_t *config)
{
    if (!store || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    const slot_keys_t *keys = keys_for_slot(slot);
    if (!keys) {
        return ESP_ERR_INVALID_ARG;
    }

#define STORE_STEP(call)                                                       \
    do {                                                                       \
        esp_err_t step_ret = (call);                                           \
        if (step_ret != ESP_OK) {                                              \
            return step_ret;                                                   \
        }                                                                      \
    } while (0)

    STORE_STEP(si_settings_store_set_u32(
        store, keys->schema, config->schema_version));
    STORE_STEP(si_settings_store_set_u32(
        store, keys->generation, config->generation));
    STORE_STEP(si_settings_store_set_u8(
        store, keys->mode, (uint8_t)config->mode));
    STORE_STEP(si_settings_store_set_u8(
        store, keys->autoip, config->autoip_fallback ? 1U : 0U));
    STORE_STEP(si_settings_store_set_string(
        store, keys->hostname, config->hostname));
    STORE_STEP(si_settings_store_set_string(
        store, keys->address, config->address));
    STORE_STEP(si_settings_store_set_string(
        store, keys->netmask, config->netmask));
    STORE_STEP(si_settings_store_set_string(
        store, keys->gateway, config->gateway));
    STORE_STEP(si_settings_store_set_string(
        store, keys->dns_primary, config->dns_primary));
    STORE_STEP(si_settings_store_set_string(
        store, keys->dns_secondary, config->dns_secondary));
#undef STORE_STEP
    return ESP_OK;
}

static esp_err_t load_status(si_settings_store_t *store,
                             si_network_settings_status_t *out)
{
    if (!store || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    esp_err_t ret = si_settings_store_get_u8(
        store, KEY_ACTIVE, &out->active_slot);
    if (ret != ESP_OK || !slot_valid(out->active_slot)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    ret = read_slot(store, out->active_slot, &out->active);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = si_settings_store_get_u8(store, KEY_STAGED, &out->staged_slot);
    if (ret == ESP_ERR_NOT_FOUND) {
        out->staged_slot = 0;
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (slot_valid(out->staged_slot) &&
        out->staged_slot != out->active_slot &&
        read_slot(store, out->staged_slot, &out->staged) == ESP_OK) {
        out->have_staged = true;
    } else {
        out->staged_slot = 0;
    }

    ret = si_settings_store_get_u8(store, KEY_PENDING, &out->pending_slot);
    if (ret == ESP_ERR_NOT_FOUND) {
        out->pending_slot = 0;
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    out->pending = out->have_staged &&
                   out->pending_slot == out->staged_slot;
    if (!out->pending) {
        out->pending_slot = 0;
    }
    return ESP_OK;
}

esp_err_t si_network_settings_initialize(
    const si_network_config_t *factory_defaults,
    si_network_settings_status_t *status_out)
{
    if (!factory_defaults ||
        !si_network_config_validate(factory_defaults, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    REQUIRE_FLASH_SAFE_STACK();

    invalidate_cached_status();

    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    if (ret != ESP_OK) {
        return ret;
    }

    si_network_settings_status_t status;
    ret = load_status(&store, &status);
    if (ret != ESP_OK) {
        ret = si_settings_store_erase_all(&store);
        if (ret == ESP_OK) {
            ret = write_slot(&store, NETWORK_SLOT_A, factory_defaults);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u8(
                &store, KEY_ACTIVE, NETWORK_SLOT_A);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u8(&store, KEY_STAGED, 0);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        if (ret == ESP_OK) {
            ret = load_status(&store, &status);
        }
    } else if (status.pending) {
        status.recovered_pending = true;
        ret = si_settings_store_set_u8(&store, KEY_STAGED, 0);
        if (ret == ESP_OK) {
            ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
        }
        if (ret == ESP_OK) {
            ret = si_settings_store_commit(&store);
        }
        if (ret == ESP_OK) {
            si_network_settings_status_t recovered;
            ret = load_status(&store, &recovered);
            if (ret == ESP_OK) {
                recovered.recovered_pending = true;
                status = recovered;
            }
        }
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        if (status_out) {
            *status_out = status;
        }
        /* recovered_pending is a one-shot boot result, not persisted state. */
        status.recovered_pending = false;
        publish_cached_status(&status);
    }
    return ret;
}

esp_err_t si_network_settings_get(si_network_settings_status_t *status_out)
{
    return copy_cached_status(status_out);
}

esp_err_t si_network_settings_stage(const si_network_config_t *config)
{
    if (!config || !si_network_config_validate(config, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    REQUIRE_FLASH_SAFE_STACK();
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    si_network_settings_status_t status = {0};
    if (ret == ESP_OK) {
        ret = load_status(&store, &status);
    }
    uint8_t target_slot = ret == ESP_OK
                              ? other_slot(status.active_slot)
                              : NETWORK_SLOT_A;
    si_network_config_t staged = *config;
    staged.generation = status.active.generation + 1U;
    if (ret == ESP_OK) {
        ret = write_slot(&store, target_slot, &staged);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_STAGED, target_slot);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        status.staged = staged;
        status.staged_slot = target_slot;
        status.pending_slot = 0;
        status.have_staged = true;
        status.pending = false;
        status.recovered_pending = false;
        publish_cached_status(&status);
    }
    return ret;
}

esp_err_t si_network_settings_mark_pending(void)
{
    REQUIRE_FLASH_SAFE_STACK();
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    si_network_settings_status_t status;
    if (ret == ESP_OK) {
        ret = load_status(&store, &status);
    }
    if (ret == ESP_OK && !status.have_staged) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(
            &store, KEY_PENDING, status.staged_slot);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        status.pending_slot = status.staged_slot;
        status.pending = true;
        status.recovered_pending = false;
        publish_cached_status(&status);
    }
    return ret;
}

esp_err_t si_network_settings_confirm(void)
{
    REQUIRE_FLASH_SAFE_STACK();
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    si_network_settings_status_t status;
    if (ret == ESP_OK) {
        ret = load_status(&store, &status);
    }
    if (ret == ESP_OK && !status.pending) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(
            &store, KEY_ACTIVE, status.pending_slot);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_STAGED, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        status.active = status.staged;
        status.active_slot = status.pending_slot;
        memset(&status.staged, 0, sizeof(status.staged));
        status.staged_slot = 0;
        status.pending_slot = 0;
        status.have_staged = false;
        status.pending = false;
        status.recovered_pending = false;
        publish_cached_status(&status);
    }
    return ret;
}

esp_err_t si_network_settings_rollback(void)
{
    REQUIRE_FLASH_SAFE_STACK();
    si_network_settings_status_t status;
    esp_err_t cache_ret = copy_cached_status(&status);
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_STAGED, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK && cache_ret == ESP_OK) {
        memset(&status.staged, 0, sizeof(status.staged));
        status.staged_slot = 0;
        status.pending_slot = 0;
        status.have_staged = false;
        status.pending = false;
        status.recovered_pending = false;
        publish_cached_status(&status);
    }
    return ret;
}

esp_err_t si_network_settings_reset(
    const si_network_config_t *factory_defaults)
{
    if (!factory_defaults ||
        !si_network_config_validate(factory_defaults, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    REQUIRE_FLASH_SAFE_STACK();
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_write(
        &store, NETWORK_SETTINGS_NAMESPACE);
    if (ret == ESP_OK) {
        ret = si_settings_store_erase_all(&store);
    }
    if (ret == ESP_OK) {
        ret = write_slot(&store, NETWORK_SLOT_A, factory_defaults);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(
            &store, KEY_ACTIVE, NETWORK_SLOT_A);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_STAGED, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_set_u8(&store, KEY_PENDING, 0);
    }
    if (ret == ESP_OK) {
        ret = si_settings_store_commit(&store);
    }
    si_settings_store_close(&store);
    if (ret == ESP_OK) {
        si_network_settings_status_t status = {
            .active = *factory_defaults,
            .active_slot = NETWORK_SLOT_A,
        };
        publish_cached_status(&status);
    }
    return ret;
}
