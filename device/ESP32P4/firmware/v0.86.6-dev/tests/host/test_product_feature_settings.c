#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_settings.h"
#include "settings_store.h"

typedef enum {
    FAKE_STORE_OPEN_FAIL = 0,
    FAKE_STORE_MISSING,
    FAKE_STORE_READ_FAIL,
    FAKE_STORE_VALUES,
} fake_store_mode_t;

static fake_store_mode_t s_mode;
static unsigned s_open_count;

esp_err_t si_settings_store_open_read(si_settings_store_t *store,
                                      const char *namespace_name)
{
    assert(store);
    assert(namespace_name);
    s_open_count++;
    if (s_mode == FAKE_STORE_OPEN_FAIL) {
        return ESP_FAIL;
    }
    if (s_mode == FAKE_STORE_MISSING) {
        return ESP_ERR_NOT_FOUND;
    }
    store->open = true;
    return ESP_OK;
}

esp_err_t si_settings_store_open_write(si_settings_store_t *store,
                                       const char *namespace_name)
{
    (void)store;
    (void)namespace_name;
    return ESP_ERR_NOT_SUPPORTED;
}

void si_settings_store_close(si_settings_store_t *store)
{
    if (store) {
        store->open = false;
    }
}

esp_err_t si_settings_store_get_u8(si_settings_store_t *store,
                                   const char *key, uint8_t *out)
{
    assert(store && store->open && key && out);
    if (s_mode == FAKE_STORE_READ_FAIL) {
        return ESP_FAIL;
    }
    if (strcmp(key, "lan_discovery") == 0) {
        *out = 0;
    } else if (strcmp(key, "agent_enabled") == 0) {
        *out = 0;
    } else if (strcmp(key, "page_context") == 0) {
        *out = 1;
    } else if (strcmp(key, "history") == 0) {
        /* Existing namespaces from an older image legitimately lack a newly
         * introduced key; that key alone must retain its product default. */
        return ESP_ERR_NOT_FOUND;
    } else if (strcmp(key, "memory") == 0) {
        *out = 1;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t si_settings_store_get_string(si_settings_store_t *store,
                                       const char *key, char *out,
                                       size_t out_size)
{
    (void)store;
    (void)key;
    (void)out;
    (void)out_size;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t si_settings_store_get_u32(si_settings_store_t *store,
                                    const char *key, uint32_t *out)
{
    (void)store;
    (void)key;
    (void)out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t si_settings_store_set_string(si_settings_store_t *store,
                                       const char *key, const char *value)
{
    (void)store;
    (void)key;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_settings_store_set_u8(si_settings_store_t *store,
                                   const char *key, uint8_t value)
{
    (void)store;
    (void)key;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_settings_store_set_u32(si_settings_store_t *store,
                                    const char *key, uint32_t value)
{
    (void)store;
    (void)key;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_settings_store_commit(si_settings_store_t *store)
{
    (void)store;
    return ESP_ERR_NOT_SUPPORTED;
}

static void assert_all_disabled(const si_product_feature_settings_t *settings)
{
    assert(!settings->lan_discovery_enabled);
    assert(!settings->embedded_agent_enabled);
    assert(!settings->page_context_enabled);
    assert(!settings->conversation_history_enabled);
    assert(!settings->long_term_memory_enabled);
}

static void test_failure_is_closed_and_retryable(void)
{
    s_mode = FAKE_STORE_OPEN_FAIL;
    assert(si_product_feature_settings_preload() == ESP_FAIL);
    si_product_feature_settings_t settings = {0};
    si_product_feature_settings_get(&settings);
    assert_all_disabled(&settings);
    assert(!si_lan_discovery_is_enabled_cached());
    assert(s_open_count == 2);

    s_mode = FAKE_STORE_VALUES;
    assert(si_product_feature_settings_preload() == ESP_OK);
    si_product_feature_settings_get(&settings);
    assert(!settings.lan_discovery_enabled);
    assert(!settings.embedded_agent_enabled);
    assert(settings.page_context_enabled);
    assert(settings.conversation_history_enabled ==
           SI_CONVERSATION_HISTORY_DEFAULT_ENABLED);
    assert(settings.long_term_memory_enabled);
    assert(s_open_count == 3);
    assert(si_product_feature_settings_preload() == ESP_OK);
    assert(s_open_count == 3);
}

static void test_missing_namespace_uses_defaults(void)
{
    s_mode = FAKE_STORE_MISSING;
    assert(si_product_feature_settings_preload() == ESP_OK);
    si_product_feature_settings_t settings = {0};
    si_product_feature_settings_get(&settings);
    assert(settings.lan_discovery_enabled == SI_LAN_DISCOVERY_DEFAULT_ENABLED);
    assert(settings.embedded_agent_enabled == SI_EMBEDDED_AGENT_DEFAULT_ENABLED);
    assert(settings.page_context_enabled == SI_PAGE_CONTEXT_DEFAULT_ENABLED);
    assert(settings.conversation_history_enabled ==
           SI_CONVERSATION_HISTORY_DEFAULT_ENABLED);
    assert(settings.long_term_memory_enabled ==
           SI_LONG_TERM_MEMORY_DEFAULT_ENABLED);
    assert(s_open_count == 1);
}

static void test_read_failure_is_closed_and_retryable(void)
{
    s_mode = FAKE_STORE_READ_FAIL;
    assert(si_product_feature_settings_preload() == ESP_FAIL);
    si_product_feature_settings_t settings = {0};
    si_product_feature_settings_get(&settings);
    assert_all_disabled(&settings);
    assert(s_open_count == 2);

    s_mode = FAKE_STORE_VALUES;
    assert(si_product_feature_settings_preload() == ESP_OK);
    assert(s_open_count == 3);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "failure-retry") == 0) {
        test_failure_is_closed_and_retryable();
    } else if (strcmp(argv[1], "missing-defaults") == 0) {
        test_missing_namespace_uses_defaults();
    } else if (strcmp(argv[1], "read-failure-retry") == 0) {
        test_read_failure_is_closed_and_retryable();
    } else {
        assert(false);
    }
    puts("product feature settings tests: PASS");
    return 0;
}
