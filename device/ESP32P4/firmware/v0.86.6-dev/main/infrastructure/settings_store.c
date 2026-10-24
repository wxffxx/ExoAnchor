#include "settings_store.h"

#include <string.h>

#include "nvs.h"

#include "flash_access.h"

/*
 * ESP-IDF disables the flash cache while NVS accesses the partition. A task
 * whose current stack lives in PSRAM cannot survive that transition, so the
 * public functions below route such callers through the flash_access proxy.
 * The direct implementations keep this check so a missed route fails closed
 * instead of reaching cache_utils.c's fatal assertion.
 */
static bool current_task_can_access_flash(void)
{
    return si_flash_access_direct();
}

static esp_err_t normalize_error(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    return error;
}

static esp_err_t validate_store(const si_settings_store_t *store,
                                bool require_write)
{
    if (!store || !store->open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (require_write && !store->writable) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!current_task_can_access_flash()) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static nvs_handle_t raw_nvs_handle(const si_settings_store_t *store)
{
    return (nvs_handle_t)store->handle;
}

static esp_err_t open_store(si_settings_store_t *store,
                            const char *namespace_name, nvs_open_mode_t mode)
{
    if (!store || !namespace_name || namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (store->open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!current_task_can_access_flash()) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t ret = normalize_error(nvs_open(namespace_name, mode, &handle));
    if (ret != ESP_OK) {
        return ret;
    }
    store->handle = (uintptr_t)handle;
    store->open = true;
    store->writable = mode == NVS_READWRITE;
    return ESP_OK;
}

static esp_err_t store_open_read_direct(si_settings_store_t *store,
                                      const char *namespace_name)
{
    return open_store(store, namespace_name, NVS_READONLY);
}

static esp_err_t store_open_write_direct(si_settings_store_t *store,
                                       const char *namespace_name)
{
    return open_store(store, namespace_name, NVS_READWRITE);
}

void si_settings_store_close(si_settings_store_t *store)
{
    if (!store || !store->open) {
        return;
    }
    nvs_close(raw_nvs_handle(store));
    memset(store, 0, sizeof(*store));
}

static esp_err_t store_get_string_direct(si_settings_store_t *store,
                                       const char *key, char *out,
                                       size_t out_size)
{
    if (!key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, false);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t length = out_size;
    return normalize_error(nvs_get_str(raw_nvs_handle(store), key, out, &length));
}

static esp_err_t store_get_string_size_direct(si_settings_store_t *store,
                                            const char *key,
                                            size_t *size_out)
{
    if (!key || !size_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, false);
    if (ret != ESP_OK) {
        return ret;
    }
    *size_out = 0;
    return normalize_error(nvs_get_str(raw_nvs_handle(store), key, NULL, size_out));
}

#define DEFINE_GETTER(name, type, nvs_fn)                                      \
    static esp_err_t store_get_##name##_direct(si_settings_store_t *store,          \
                                            const char *key, type *out)          \
    {                                                                           \
        if (!key || !out) {                                                     \
            return ESP_ERR_INVALID_ARG;                                         \
        }                                                                       \
        esp_err_t ret = validate_store(store, false);                            \
        return ret == ESP_OK ? normalize_error(                                \
            nvs_fn(raw_nvs_handle(store), key, out)) : ret;                     \
    }

DEFINE_GETTER(u8, uint8_t, nvs_get_u8)
DEFINE_GETTER(u16, uint16_t, nvs_get_u16)
DEFINE_GETTER(u32, uint32_t, nvs_get_u32)
DEFINE_GETTER(i32, int32_t, nvs_get_i32)

#undef DEFINE_GETTER

static esp_err_t store_set_string_direct(si_settings_store_t *store,
                                       const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_set_str(raw_nvs_handle(store), key, value)) : ret;
}

#define DEFINE_SETTER(name, type, nvs_fn)                                      \
    static esp_err_t store_set_##name##_direct(si_settings_store_t *store,          \
                                            const char *key, type value)         \
    {                                                                           \
        if (!key) {                                                             \
            return ESP_ERR_INVALID_ARG;                                         \
        }                                                                       \
        esp_err_t ret = validate_store(store, true);                             \
        return ret == ESP_OK ? normalize_error(                                \
            nvs_fn(raw_nvs_handle(store), key, value)) : ret;                   \
    }

DEFINE_SETTER(u8, uint8_t, nvs_set_u8)
DEFINE_SETTER(u16, uint16_t, nvs_set_u16)
DEFINE_SETTER(u32, uint32_t, nvs_set_u32)
DEFINE_SETTER(i32, int32_t, nvs_set_i32)

#undef DEFINE_SETTER

static esp_err_t store_erase_key_direct(si_settings_store_t *store,
                                      const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_erase_key(raw_nvs_handle(store), key)) : ret;
}

static esp_err_t store_erase_all_direct(si_settings_store_t *store)
{
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(
        nvs_erase_all(raw_nvs_handle(store))) : ret;
}

static esp_err_t store_commit_direct(si_settings_store_t *store)
{
    esp_err_t ret = validate_store(store, true);
    return ret == ESP_OK ? normalize_error(nvs_commit(raw_nvs_handle(store))) : ret;
}

/* Public entry points: run the direct implementation where flash access is
 * safe, via the flash_access proxy for PSRAM-stack callers. */
typedef enum {
    STORE_OP_OPEN_READ,
    STORE_OP_OPEN_WRITE,
    STORE_OP_GET_STRING,
    STORE_OP_GET_STRING_SIZE,
    STORE_OP_GET_U8,
    STORE_OP_GET_U16,
    STORE_OP_GET_U32,
    STORE_OP_GET_I32,
    STORE_OP_SET_STRING,
    STORE_OP_SET_U8,
    STORE_OP_SET_U16,
    STORE_OP_SET_U32,
    STORE_OP_SET_I32,
    STORE_OP_ERASE_KEY,
    STORE_OP_ERASE_ALL,
    STORE_OP_COMMIT,
} store_op_kind_t;

typedef struct {
    store_op_kind_t kind;
    si_settings_store_t *store;
    const char *name;
    const char *text;
    char *out;
    size_t out_size;
    size_t *size_out;
    void *number_out;
    uint32_t number;
} store_op_t;

static esp_err_t store_op_run(void *ctx)
{
    store_op_t *op = (store_op_t *)ctx;
    switch (op->kind) {
    case STORE_OP_OPEN_READ:
        return store_open_read_direct(op->store, op->name);
    case STORE_OP_OPEN_WRITE:
        return store_open_write_direct(op->store, op->name);
    case STORE_OP_GET_STRING:
        return store_get_string_direct(op->store, op->name, op->out, op->out_size);
    case STORE_OP_GET_STRING_SIZE:
        return store_get_string_size_direct(op->store, op->name, op->size_out);
    case STORE_OP_GET_U8:
        return store_get_u8_direct(op->store, op->name, (uint8_t *)op->number_out);
    case STORE_OP_GET_U16:
        return store_get_u16_direct(op->store, op->name, (uint16_t *)op->number_out);
    case STORE_OP_GET_U32:
        return store_get_u32_direct(op->store, op->name, (uint32_t *)op->number_out);
    case STORE_OP_GET_I32:
        return store_get_i32_direct(op->store, op->name, (int32_t *)op->number_out);
    case STORE_OP_SET_STRING:
        return store_set_string_direct(op->store, op->name, op->text);
    case STORE_OP_SET_U8:
        return store_set_u8_direct(op->store, op->name, (uint8_t)op->number);
    case STORE_OP_SET_U16:
        return store_set_u16_direct(op->store, op->name, (uint16_t)op->number);
    case STORE_OP_SET_U32:
        return store_set_u32_direct(op->store, op->name, op->number);
    case STORE_OP_SET_I32:
        return store_set_i32_direct(op->store, op->name, (int32_t)op->number);
    case STORE_OP_ERASE_KEY:
        return store_erase_key_direct(op->store, op->name);
    case STORE_OP_ERASE_ALL:
        return store_erase_all_direct(op->store);
    case STORE_OP_COMMIT:
        return store_commit_direct(op->store);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t store_op_call(store_op_t *op)
{
    return si_flash_access_call(store_op_run, op);
}

esp_err_t si_settings_store_open_read(si_settings_store_t *store,
                                      const char *namespace_name)
{
    store_op_t op = {.kind = STORE_OP_OPEN_READ, .store = store, .name = namespace_name};
    return store_op_call(&op);
}

esp_err_t si_settings_store_open_write(si_settings_store_t *store,
                                       const char *namespace_name)
{
    store_op_t op = {.kind = STORE_OP_OPEN_WRITE, .store = store, .name = namespace_name};
    return store_op_call(&op);
}

esp_err_t si_settings_store_get_string(si_settings_store_t *store,
                                       const char *key, char *out,
                                       size_t out_size)
{
    store_op_t op = {.kind = STORE_OP_GET_STRING, .store = store, .name = key,
                     .out = out, .out_size = out_size};
    return store_op_call(&op);
}

esp_err_t si_settings_store_get_string_size(si_settings_store_t *store,
                                            const char *key,
                                            size_t *size_out)
{
    store_op_t op = {.kind = STORE_OP_GET_STRING_SIZE, .store = store,
                     .name = key, .size_out = size_out};
    return store_op_call(&op);
}

#define DEFINE_PUBLIC_GETTER(suffix, type, op_kind)                              \
    esp_err_t si_settings_store_get_##suffix(si_settings_store_t *store,          \
                                            const char *key, type *out)          \
    {                                                                           \
        store_op_t op = {.kind = op_kind, .store = store, .name = key,          \
                         .number_out = out};                                    \
        return store_op_call(&op);                                              \
    }

DEFINE_PUBLIC_GETTER(u8, uint8_t, STORE_OP_GET_U8)
DEFINE_PUBLIC_GETTER(u16, uint16_t, STORE_OP_GET_U16)
DEFINE_PUBLIC_GETTER(u32, uint32_t, STORE_OP_GET_U32)
DEFINE_PUBLIC_GETTER(i32, int32_t, STORE_OP_GET_I32)

#undef DEFINE_PUBLIC_GETTER

esp_err_t si_settings_store_set_string(si_settings_store_t *store,
                                       const char *key, const char *value)
{
    store_op_t op = {.kind = STORE_OP_SET_STRING, .store = store, .name = key,
                     .text = value};
    return store_op_call(&op);
}

#define DEFINE_PUBLIC_SETTER(suffix, type, op_kind)                              \
    esp_err_t si_settings_store_set_##suffix(si_settings_store_t *store,          \
                                            const char *key, type value)         \
    {                                                                           \
        store_op_t op = {.kind = op_kind, .store = store, .name = key,          \
                         .number = (uint32_t)value};                            \
        return store_op_call(&op);                                              \
    }

DEFINE_PUBLIC_SETTER(u8, uint8_t, STORE_OP_SET_U8)
DEFINE_PUBLIC_SETTER(u16, uint16_t, STORE_OP_SET_U16)
DEFINE_PUBLIC_SETTER(u32, uint32_t, STORE_OP_SET_U32)
DEFINE_PUBLIC_SETTER(i32, int32_t, STORE_OP_SET_I32)

#undef DEFINE_PUBLIC_SETTER

esp_err_t si_settings_store_erase_key(si_settings_store_t *store,
                                      const char *key)
{
    store_op_t op = {.kind = STORE_OP_ERASE_KEY, .store = store, .name = key};
    return store_op_call(&op);
}

esp_err_t si_settings_store_erase_all(si_settings_store_t *store)
{
    store_op_t op = {.kind = STORE_OP_ERASE_ALL, .store = store};
    return store_op_call(&op);
}

esp_err_t si_settings_store_commit(si_settings_store_t *store)
{
    store_op_t op = {.kind = STORE_OP_COMMIT, .store = store};
    return store_op_call(&op);
}
