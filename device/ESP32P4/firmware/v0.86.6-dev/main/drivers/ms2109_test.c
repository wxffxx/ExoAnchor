#include "ms2109_test.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"

#if SI_CFG_MS2109_TEST_ENABLED
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ms2109-test";

#define SI_AT24C16_BASE_ADDRESS 0x50U
#define SI_AT24C16_BLOCK_COUNT 8U
#define SI_AT24C16_I2C_HZ 100000U
#define SI_AT24C16_TRANSFER_TIMEOUT_MS 100
#define SI_AT24C16_WRITE_TIMEOUT_MS 25
#define SI_MS2109_POWER_SETTLE_MS 100U
#define SI_MS2109_RAIL_SEQUENCE_MS 10U
#define SI_MS2109_MAX_SEQUENCE_DELAY_MS 500U
#define SI_MS2109_I2C_RECOVERY_PULSES 9U

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t devices[SI_AT24C16_BLOCK_COUNT];
    bool restore_power_on;
} si_ms2109_transaction_t;

static SemaphoreHandle_t s_lock;
static si_ms2109_test_status_t s_status = {
    .enabled = true,
    .switch_gpio = SI_CFG_MS2109_SWITCH_GPIO,
    .core_enable_gpio = SI_CFG_MS2109_CORE_ENABLE_GPIO,
    .switch_output_level = -1,
    .core_enable_output_level = -1,
    .wp_gpio = SI_CFG_MS2109_EEPROM_WP_GPIO,
    .scl_gpio = SI_CFG_MS2109_EEPROM_SCL_GPIO,
    .sda_gpio = SI_CFG_MS2109_EEPROM_SDA_GPIO,
    .last_scl_level = -1,
    .last_sda_level = -1,
    .last_result = ESP_ERR_INVALID_STATE,
};

static int active_level(bool on, bool active_high)
{
    return active_high ? (on ? 1 : 0) : (on ? 0 : 1);
}

static void sample_power_control_levels_locked(void)
{
    s_status.switch_output_level =
        gpio_get_level(SI_CFG_MS2109_SWITCH_GPIO);
    s_status.core_enable_output_level =
        gpio_get_level(SI_CFG_MS2109_CORE_ENABLE_GPIO);
}

static esp_err_t set_power_on_sequence_locked(
    si_ms2109_power_sequence_t sequence, uint32_t delay_ms)
{
    ESP_RETURN_ON_FALSE(
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_SWITCH_GPIO) &&
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_CORE_ENABLE_GPIO),
        ESP_ERR_NOT_SUPPORTED, TAG, "invalid MS power GPIO");

    ESP_RETURN_ON_FALSE(
        sequence == SI_MS2109_POWER_SEQUENCE_3V3_FIRST ||
        sequence == SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE,
        ESP_ERR_INVALID_ARG, TAG, "invalid MS power sequence");
    ESP_RETURN_ON_FALSE(delay_ms <= SI_MS2109_MAX_SEQUENCE_DELAY_MS,
                        ESP_ERR_INVALID_ARG, TAG,
                        "MS power sequence delay too large");

    esp_err_t ret = ESP_OK;
    if (sequence == SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE) {
        /*
         * U10 VIN is 3V3_MS, so this does not create a 1.2 V rail while the
         * load switch is off. It makes U10 start as soon as 3V3_MS rises,
         * matching the vendor reference design's common-input ramp.
         */
        ret = gpio_set_level(
            SI_CFG_MS2109_CORE_ENABLE_GPIO,
            active_level(true, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
        if (ret == ESP_OK && delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        if (ret == ESP_OK) {
            ret = gpio_set_level(
                SI_CFG_MS2109_SWITCH_GPIO,
                active_level(true, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        }
        if (ret != ESP_OK) {
            (void)gpio_set_level(
                SI_CFG_MS2109_CORE_ENABLE_GPIO,
                active_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
        }
    } else {
        ret = gpio_set_level(
            SI_CFG_MS2109_SWITCH_GPIO,
            active_level(true, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        if (ret == ESP_OK && delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        if (ret == ESP_OK) {
            ret = gpio_set_level(
                SI_CFG_MS2109_CORE_ENABLE_GPIO,
                active_level(true, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
        }
        if (ret != ESP_OK) {
            (void)gpio_set_level(
                SI_CFG_MS2109_SWITCH_GPIO,
                active_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
        }
    }
    s_status.power_on = ret == ESP_OK;
    s_status.last_power_sequence = sequence;
    s_status.last_sequence_delay_ms = delay_ms;
    sample_power_control_levels_locked();
    return ret;
}

static esp_err_t set_power_locked(bool on)
{
    if (on) {
        return set_power_on_sequence_locked(
            SI_MS2109_POWER_SEQUENCE_3V3_FIRST,
            SI_MS2109_RAIL_SEQUENCE_MS);
    }

    ESP_RETURN_ON_FALSE(
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_SWITCH_GPIO) &&
        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_CORE_ENABLE_GPIO),
        ESP_ERR_NOT_SUPPORTED, TAG, "invalid MS power GPIO");
    esp_err_t ret = gpio_set_level(
        SI_CFG_MS2109_CORE_ENABLE_GPIO,
        active_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH));
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SI_MS2109_RAIL_SEQUENCE_MS));
        ret = gpio_set_level(
            SI_CFG_MS2109_SWITCH_GPIO,
            active_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH));
    }
    s_status.power_on = false;
    sample_power_control_levels_locked();
    return ret;
}

static esp_err_t set_write_protect_locked(bool protect)
{
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_EEPROM_WP_GPIO),
                        ESP_ERR_NOT_SUPPORTED, TAG, "invalid EEPROM WP GPIO");
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SI_CFG_MS2109_EEPROM_WP_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = protect ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_set_level(SI_CFG_MS2109_EEPROM_WP_GPIO, protect ? 1 : 0);
    if (ret == ESP_OK) {
        ret = gpio_config(&config);
    }
    if (ret == ESP_OK) {
        s_status.write_protected = protect;
    }
    return ret;
}

static esp_err_t prepare_eeprom_lines_locked(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << SI_CFG_MS2109_EEPROM_SCL_GPIO) |
                        (1ULL << SI_CFG_MS2109_EEPROM_SDA_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_set_level(SI_CFG_MS2109_EEPROM_SCL_GPIO, 1), TAG,
        "release EEPROM SCL");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(SI_CFG_MS2109_EEPROM_SDA_GPIO, 1), TAG,
        "release EEPROM SDA");
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG,
                        "configure EEPROM recovery pins");
    esp_rom_delay_us(10);

    int scl = gpio_get_level(SI_CFG_MS2109_EEPROM_SCL_GPIO);
    int sda = gpio_get_level(SI_CFG_MS2109_EEPROM_SDA_GPIO);
    if (scl && !sda) {
        for (size_t i = 0; i < SI_MS2109_I2C_RECOVERY_PULSES; ++i) {
            ESP_RETURN_ON_ERROR(
                gpio_set_level(SI_CFG_MS2109_EEPROM_SCL_GPIO, 0), TAG,
                "drive EEPROM recovery clock low");
            esp_rom_delay_us(10);
            ESP_RETURN_ON_ERROR(
                gpio_set_level(SI_CFG_MS2109_EEPROM_SCL_GPIO, 1), TAG,
                "release EEPROM recovery clock");
            esp_rom_delay_us(10);
        }
        ESP_RETURN_ON_ERROR(
            gpio_set_level(SI_CFG_MS2109_EEPROM_SDA_GPIO, 0), TAG,
            "start EEPROM recovery STOP");
        esp_rom_delay_us(10);
        ESP_RETURN_ON_ERROR(
            gpio_set_level(SI_CFG_MS2109_EEPROM_SCL_GPIO, 1), TAG,
            "clock EEPROM recovery STOP");
        esp_rom_delay_us(10);
        ESP_RETURN_ON_ERROR(
            gpio_set_level(SI_CFG_MS2109_EEPROM_SDA_GPIO, 1), TAG,
            "complete EEPROM recovery STOP");
        esp_rom_delay_us(10);
    }
    return ESP_OK;
}

static void transaction_close(si_ms2109_transaction_t *transaction)
{
    if (!transaction) {
        return;
    }
    for (size_t i = 0; i < SI_AT24C16_BLOCK_COUNT; ++i) {
        if (transaction->devices[i]) {
            esp_err_t ret = i2c_master_bus_rm_device(transaction->devices[i]);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "remove AT24C16 block %u: %s", (unsigned)i,
                         esp_err_to_name(ret));
            }
            transaction->devices[i] = NULL;
        }
    }
    if (transaction->bus) {
        esp_err_t ret = i2c_del_master_bus(transaction->bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "release EEPROM I2C bus: %s", esp_err_to_name(ret));
        }
        transaction->bus = NULL;
    }
    esp_err_t protect_ret = set_write_protect_locked(true);
    if (protect_ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to restore EEPROM write protection: %s",
                 esp_err_to_name(protect_ret));
    }
    if (s_status.power_on != transaction->restore_power_on) {
        esp_err_t power_ret = set_power_locked(transaction->restore_power_on);
        if (power_ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore MS2109 power state: %s",
                     esp_err_to_name(power_ret));
        }
    }
}

static esp_err_t transaction_open(si_ms2109_transaction_t *transaction)
{
    ESP_RETURN_ON_FALSE(transaction, ESP_ERR_INVALID_ARG, TAG,
                        "missing EEPROM transaction");
    memset(transaction, 0, sizeof(*transaction));
    transaction->restore_power_on = s_status.power_on;

    /*
     * V2.4a6 ties R36/R37 to 3V3_MS. With MS2109 off, both shared I2C lines
     * are held low on the prototype, so the P4 cannot arbitrate the EEPROM
     * bus. Keep the MS2109 fully powered and wait until its boot-time EEPROM
     * read has completed before attaching the P4 master to the idle bus.
     */
    esp_err_t ret = ESP_OK;
    if (!transaction->restore_power_on) {
        ret = set_power_locked(true);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(SI_MS2109_POWER_SETTLE_MS));
    ret = prepare_eeprom_lines_locked();
    if (ret != ESP_OK) {
        transaction_close(transaction);
        return ret;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = (i2c_port_num_t)-1,
        .sda_io_num = SI_CFG_MS2109_EEPROM_SDA_GPIO,
        .scl_io_num = SI_CFG_MS2109_EEPROM_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        // Board pull-ups remain powered during isolation; internal pull-ups
        // provide a safe fallback during rail transitions.
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_config, &transaction->bus);
    if (ret != ESP_OK) {
        transaction_close(transaction);
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    s_status.last_scl_level = gpio_get_level(SI_CFG_MS2109_EEPROM_SCL_GPIO);
    s_status.last_sda_level = gpio_get_level(SI_CFG_MS2109_EEPROM_SDA_GPIO);
    if (!s_status.last_scl_level || !s_status.last_sda_level) {
        ESP_LOGW(TAG, "EEPROM bus held low after isolation: SCL=%d SDA=%d",
                 s_status.last_scl_level, s_status.last_sda_level);
    }

    for (size_t i = 0; i < SI_AT24C16_BLOCK_COUNT; ++i) {
        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SI_AT24C16_BASE_ADDRESS + i,
            .scl_speed_hz = SI_AT24C16_I2C_HZ,
            .scl_wait_us = 0,
            .flags.disable_ack_check = false,
        };
        ret = i2c_master_bus_add_device(transaction->bus, &device_config,
                                        &transaction->devices[i]);
        if (ret != ESP_OK) {
            transaction_close(transaction);
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t probe_locked(si_ms2109_transaction_t *transaction,
                              uint8_t *address_mask)
{
    *address_mask = 0;
    for (size_t i = 0; i < SI_AT24C16_BLOCK_COUNT; ++i) {
        if (i2c_master_probe(transaction->bus,
                             SI_AT24C16_BASE_ADDRESS + i, 20) == ESP_OK) {
            *address_mask |= 1U << i;
        }
    }
    return *address_mask == 0xffU ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t read_locked(si_ms2109_transaction_t *transaction,
                             size_t offset, uint8_t *data, size_t size)
{
    while (size > 0) {
        size_t block = offset >> 8;
        uint8_t word_address = (uint8_t)(offset & 0xffU);
        size_t chunk = 256U - word_address;
        if (chunk > size) {
            chunk = size;
        }
        esp_err_t ret = i2c_master_transmit_receive(
            transaction->devices[block], &word_address, 1, data, chunk,
            SI_AT24C16_TRANSFER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
        offset += chunk;
        data += chunk;
        size -= chunk;
    }
    return ESP_OK;
}

static esp_err_t wait_write_complete(si_ms2109_transaction_t *transaction,
                                     size_t block)
{
    for (int elapsed = 0; elapsed < SI_AT24C16_WRITE_TIMEOUT_MS; ++elapsed) {
        esp_err_t ret = i2c_master_probe(
            transaction->bus, SI_AT24C16_BASE_ADDRESS + block, 1);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_TIMEOUT) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t write_locked(si_ms2109_transaction_t *transaction,
                              const uint8_t *data, size_t size)
{
    size_t offset = 0;
    uint8_t page[SI_MS2109_AT24C16_PAGE_SIZE + 1U];
    while (offset < size) {
        size_t block = offset >> 8;
        uint8_t word_address = (uint8_t)(offset & 0xffU);
        size_t chunk = SI_MS2109_AT24C16_PAGE_SIZE -
                       (word_address % SI_MS2109_AT24C16_PAGE_SIZE);
        if (chunk > size - offset) {
            chunk = size - offset;
        }
        page[0] = word_address;
        memcpy(page + 1, data + offset, chunk);
        esp_err_t ret = i2c_master_transmit(transaction->devices[block], page,
                                            chunk + 1,
                                            SI_AT24C16_TRANSFER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = wait_write_complete(transaction, block);
        if (ret != ESP_OK) {
            return ret;
        }
        offset += chunk;
    }
    return ESP_OK;
}

esp_err_t si_ms2109_test_init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_SWITCH_GPIO) &&
                        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_CORE_ENABLE_GPIO) &&
                        GPIO_IS_VALID_OUTPUT_GPIO(SI_CFG_MS2109_EEPROM_WP_GPIO) &&
                        GPIO_IS_VALID_GPIO(SI_CFG_MS2109_EEPROM_SCL_GPIO) &&
                        GPIO_IS_VALID_GPIO(SI_CFG_MS2109_EEPROM_SDA_GPIO),
                        ESP_ERR_INVALID_ARG, TAG, "invalid V2.4 MS test GPIO map");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create MS test lock");

    const gpio_config_t switch_config = {
        .pin_bit_mask = (1ULL << SI_CFG_MS2109_SWITCH_GPIO) |
                        (1ULL << SI_CFG_MS2109_CORE_ENABLE_GPIO),
        /* Keep the input buffer enabled so status can report the pad level. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_set_level(
            SI_CFG_MS2109_SWITCH_GPIO,
            active_level(false, SI_CFG_MS2109_SWITCH_ACTIVE_HIGH)), TAG,
        "preset MS 3.3 V rail OFF");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(
            SI_CFG_MS2109_CORE_ENABLE_GPIO,
            active_level(false, SI_CFG_MS2109_CORE_ENABLE_ACTIVE_HIGH)), TAG,
        "preset MS core rail OFF");
    ESP_RETURN_ON_ERROR(gpio_config(&switch_config), TAG,
                        "configure MS power GPIOs");
    ESP_RETURN_ON_ERROR(set_power_locked(true), TAG,
                        "enable MS power rails");
    ESP_RETURN_ON_ERROR(set_write_protect_locked(true), TAG,
                        "protect EEPROM");
    s_status.initialized = true;
    s_status.last_result = ESP_OK;
    ESP_LOGI(TAG,
             "V2.4 MS test ready 3v3=GPIO%d 1v2=GPIO%d wp=GPIO%d scl=GPIO%d sda=GPIO%d",
             SI_CFG_MS2109_SWITCH_GPIO, SI_CFG_MS2109_CORE_ENABLE_GPIO,
             SI_CFG_MS2109_EEPROM_WP_GPIO,
             SI_CFG_MS2109_EEPROM_SCL_GPIO, SI_CFG_MS2109_EEPROM_SDA_GPIO);
    return ESP_OK;
}

void si_ms2109_test_get_status(si_ms2109_test_status_t *status)
{
    if (!status) {
        return;
    }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        sample_power_control_levels_locked();
        *status = s_status;
        xSemaphoreGive(s_lock);
    } else {
        *status = s_status;
    }
}

esp_err_t si_ms2109_test_set_power(bool on)
{
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");
    esp_err_t ret = set_power_locked(on);
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_test_cycle_power(uint32_t off_time_ms)
{
    if (off_time_ms < 10U || off_time_ms > 5000U) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");
    esp_err_t ret = set_power_locked(false);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(off_time_ms));
        ret = set_power_locked(true);
    }
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_test_cycle_power_sequence(
    si_ms2109_power_sequence_t sequence, uint32_t sequence_delay_ms,
    uint32_t off_time_ms)
{
    if (off_time_ms < 10U || off_time_ms > 5000U ||
        sequence_delay_ms > SI_MS2109_MAX_SEQUENCE_DELAY_MS ||
        (sequence != SI_MS2109_POWER_SEQUENCE_3V3_FIRST &&
         sequence != SI_MS2109_POWER_SEQUENCE_CORE_PRE_ENABLE)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");
    esp_err_t ret = set_power_locked(false);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(off_time_ms));
        ret = set_power_on_sequence_locked(sequence, sequence_delay_ms);
    }
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_test_probe_eeprom(uint8_t *address_mask)
{
    ESP_RETURN_ON_FALSE(address_mask, ESP_ERR_INVALID_ARG, TAG,
                        "missing probe mask");
    *address_mask = 0;
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");

    si_ms2109_transaction_t transaction;
    esp_err_t ret = transaction_open(&transaction);
    if (ret == ESP_OK) {
        ret = probe_locked(&transaction, address_mask);
        transaction_close(&transaction);
    }
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_test_read_eeprom(size_t offset, uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(data && size > 0 && offset < SI_MS2109_AT24C16_SIZE &&
                        size <= SI_MS2109_AT24C16_SIZE - offset,
                        ESP_ERR_INVALID_ARG, TAG, "invalid EEPROM read range");
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");

    si_ms2109_transaction_t transaction;
    esp_err_t ret = transaction_open(&transaction);
    if (ret == ESP_OK) {
        ret = read_locked(&transaction, offset, data, size);
        transaction_close(&transaction);
    }
    s_status.operation_count++;
    s_status.last_result = ret;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t si_ms2109_test_program_eeprom(const uint8_t *image, size_t size,
                                        uint32_t *crc32, bool *verified)
{
    ESP_RETURN_ON_FALSE(image && size == SI_MS2109_AT24C16_SIZE,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "AT24C16 image must be exactly 2048 bytes");
    ESP_RETURN_ON_FALSE(s_status.initialized && s_lock, ESP_ERR_INVALID_STATE,
                        TAG, "MS test not initialized");
    if (crc32) {
        *crc32 = 0;
    }
    if (verified) {
        *verified = false;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "MS test busy");

    uint8_t *readback = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!readback) {
        readback = malloc(size);
    }
    esp_err_t ret = readback ? ESP_OK : ESP_ERR_NO_MEM;
    si_ms2109_transaction_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    if (ret == ESP_OK) {
        ret = transaction_open(&transaction);
    }
    uint8_t address_mask = 0;
    if (ret == ESP_OK) {
        ret = probe_locked(&transaction, &address_mask);
    }
    if (ret == ESP_OK) {
        ret = set_write_protect_locked(false);
    }
    if (ret == ESP_OK) {
        ret = write_locked(&transaction, image, size);
    }
    if (ret == ESP_OK) {
        ret = set_write_protect_locked(true);
    }
    if (ret == ESP_OK) {
        ret = read_locked(&transaction, 0, readback, size);
    }

    uint32_t image_crc = esp_rom_crc32_le(0, image, size);
    bool matches = ret == ESP_OK && memcmp(image, readback, size) == 0;
    if (ret == ESP_OK && !matches) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (crc32) {
        *crc32 = image_crc;
    }
    if (verified) {
        *verified = matches;
    }

    transaction_close(&transaction);
    free(readback);
    s_status.operation_count++;
    s_status.last_programmed_bytes = ret == ESP_OK ? size : 0;
    s_status.last_crc32 = image_crc;
    s_status.last_verified = matches;
    s_status.last_result = ret;
    ESP_LOGI(TAG, "EEPROM program bytes=%u crc32=%08" PRIx32
                  " verified=%d result=%s",
             (unsigned)size, image_crc, matches, esp_err_to_name(ret));
    xSemaphoreGive(s_lock);
    return ret;
}

#else

esp_err_t si_ms2109_test_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void si_ms2109_test_get_status(si_ms2109_test_status_t *status)
{
    if (status) {
        memset(status, 0, sizeof(*status));
        status->switch_gpio = SI_CFG_MS2109_SWITCH_GPIO;
        status->core_enable_gpio = SI_CFG_MS2109_CORE_ENABLE_GPIO;
        status->switch_output_level = -1;
        status->core_enable_output_level = -1;
        status->wp_gpio = SI_CFG_MS2109_EEPROM_WP_GPIO;
        status->scl_gpio = SI_CFG_MS2109_EEPROM_SCL_GPIO;
        status->sda_gpio = SI_CFG_MS2109_EEPROM_SDA_GPIO;
        status->last_scl_level = -1;
        status->last_sda_level = -1;
        status->last_result = ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t si_ms2109_test_set_power(bool on)
{
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_test_cycle_power(uint32_t off_time_ms)
{
    (void)off_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_test_cycle_power_sequence(
    si_ms2109_power_sequence_t sequence, uint32_t sequence_delay_ms,
    uint32_t off_time_ms)
{
    (void)sequence;
    (void)sequence_delay_ms;
    (void)off_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_test_probe_eeprom(uint8_t *address_mask)
{
    if (address_mask) {
        *address_mask = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_test_read_eeprom(size_t offset, uint8_t *data, size_t size)
{
    (void)offset;
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_ms2109_test_program_eeprom(const uint8_t *image, size_t size,
                                        uint32_t *crc32, bool *verified)
{
    (void)image;
    (void)size;
    if (crc32) {
        *crc32 = 0;
    }
    if (verified) {
        *verified = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
