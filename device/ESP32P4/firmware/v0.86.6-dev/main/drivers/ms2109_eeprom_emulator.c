#include "ms2109_eeprom_emulator.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"

#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/i2c_ll.h"
#include "i2c_private.h"

static const char *TAG = "ms2109-eeprom";

#define SI_MS2109_EEPROM_ADDRESS       0x50
#define SI_MS2109_EEPROM_SIZE          256U
#define SI_MS2109_EEPROM_RX_DEPTH      32U
#define SI_MS2109_EEPROM_TX_DEPTH      512U
#define SI_MS2109_H3_EVEN_GPIO2        GPIO_NUM_2
#define SI_MS2109_H3_EVEN_GPIO3        GPIO_NUM_3

/*
 * kraln/macrosilicon_firmware minimal-test EEPROM image, bytes 0x000-0x054.
 * The remaining bytes in the emulated 256-byte address space are 0xff.
 *
 * Upstream 2 KiB image SHA-256:
 * 02e3b31d2d6c75802167bb4b76a3ce3afd9df0734b81ac7ae70a111d4302dbab
 */
static const uint8_t s_minimal_test_prefix[] = {
    0xa5, 0x5a, 0x00, 0x21, 0x05, 0x10, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x20, 0x20, 0x07, 0x07,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x90, 0xde, 0x05, 0xeb, 0xf0, 0xa3, 0xea, 0xf0,
    0xa3, 0xe9, 0xf0, 0x12, 0x63, 0x45, 0x22, 0x00,
    0x22, 0x26, 0x5e, 0x09, 0x67,
};

typedef struct {
    i2c_slave_dev_handle_t handle;
    portMUX_TYPE lock;
    uint8_t image[SI_MS2109_EEPROM_SIZE];
    uint8_t pointer;
    uint8_t hardware_pointer;
    uint8_t hardware_fifo_raddr;
    uint8_t stream_pointer;
    bool hardware_pointer_valid;
    bool hardware_read;
    uint32_t scl_falling_edges;
    uint32_t sda_falling_edges;
    uint32_t h3_gpio2_falling_edges;
    uint32_t h3_gpio3_falling_edges;
    uint32_t receive_count;
    uint32_t request_count;
} si_ms2109_eeprom_state_t;

static si_ms2109_eeprom_state_t s_eeprom = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void IRAM_ATTR load_hardware_window(
    si_ms2109_eeprom_state_t *state,
    i2c_dev_t *hardware,
    uint8_t pointer)
{
    for (uint8_t index = 0; index < SOC_I2C_FIFO_LEN; ++index) {
        uint8_t address = (uint8_t)(pointer + index);
        hardware->txfifo_mem[address & (SOC_I2C_FIFO_LEN - 1U)] =
            state->image[address];
    }
}

static void IRAM_ATTR on_scl_falling_edge(void *arg)
{
    si_ms2109_eeprom_state_t *state = arg;
    state->scl_falling_edges++;

    if (!state->handle) {
        return;
    }

    i2c_slave_dev_t *device = (i2c_slave_dev_t *)state->handle;
    i2c_dev_t *hardware = device->base->hal.dev;
    uint8_t pointer = hardware->fifo_st.slave_rw_point;
    uint8_t fifo_raddr = hardware->fifo_st.txfifo_raddr;
    bool read = hardware->sr.slave_rw != 0;

    if (read && !state->hardware_read) {
        state->request_count++;
        load_hardware_window(state, hardware, pointer);
        state->hardware_pointer = pointer;
        state->hardware_pointer_valid = true;
        state->pointer = pointer;
        state->stream_pointer = pointer;
        state->hardware_fifo_raddr =
            pointer & (SOC_I2C_FIFO_LEN - 1U);
    } else if (!read && state->hardware_read) {
        state->receive_count++;
        /*
         * The preceding stream may have replaced RAM slots with future data.
         * Restore the current random-read window at the start of every write.
         */
        load_hardware_window(state, hardware, pointer);
    }

    if (!read &&
        (!state->hardware_pointer_valid ||
         pointer != state->hardware_pointer)) {
        load_hardware_window(state, hardware, pointer);
        state->hardware_pointer = pointer;
        state->hardware_pointer_valid = true;
        state->pointer = pointer;
    }

    if (read) {
        uint8_t advance = (uint8_t)(
            (fifo_raddr - state->hardware_fifo_raddr) &
            (SOC_I2C_FIFO_LEN - 1U));
        while (advance-- > 0) {
            uint8_t consumed = state->stream_pointer++;
            hardware->txfifo_mem[
                consumed & (SOC_I2C_FIFO_LEN - 1U)] =
                state->image[(uint8_t)(consumed + SOC_I2C_FIFO_LEN)];
        }
        state->hardware_fifo_raddr = fifo_raddr;
        state->pointer = state->stream_pointer;
    }

    state->hardware_read = read;
}

static void IRAM_ATTR on_sda_falling_edge(void *arg)
{
    si_ms2109_eeprom_state_t *state = arg;
    state->sda_falling_edges++;
}

static void IRAM_ATTR on_h3_gpio2_falling_edge(void *arg)
{
    si_ms2109_eeprom_state_t *state = arg;
    state->h3_gpio2_falling_edges++;
}

static void IRAM_ATTR on_h3_gpio3_falling_edge(void *arg)
{
    si_ms2109_eeprom_state_t *state = arg;
    state->h3_gpio3_falling_edges++;
}

static esp_err_t install_bus_edge_counters(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(SI_CFG_MS2109_EEPROM_SCL_GPIO, GPIO_INTR_NEGEDGE),
        TAG, "failed to configure SCL edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(SI_CFG_MS2109_EEPROM_SDA_GPIO, GPIO_INTR_NEGEDGE),
        TAG, "failed to configure SDA edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            SI_CFG_MS2109_EEPROM_SCL_GPIO, on_scl_falling_edge, &s_eeprom),
        TAG, "failed to install SCL edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            SI_CFG_MS2109_EEPROM_SDA_GPIO, on_sda_falling_edge, &s_eeprom),
        TAG, "failed to install SDA edge counter");

    /*
     * H3 is a two-column header. Pins 2/4 (GPIO3/GPIO2) sit immediately next
     * to the intended pins 1/3. Probe them as inputs to diagnose a one-column
     * cable offset without driving either candidate line.
     */
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(SI_MS2109_H3_EVEN_GPIO2, GPIO_MODE_INPUT),
        TAG, "failed to configure H3 GPIO2 probe");
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(SI_MS2109_H3_EVEN_GPIO3, GPIO_MODE_INPUT),
        TAG, "failed to configure H3 GPIO3 probe");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(SI_MS2109_H3_EVEN_GPIO2, GPIO_INTR_NEGEDGE),
        TAG, "failed to configure H3 GPIO2 edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(SI_MS2109_H3_EVEN_GPIO3, GPIO_INTR_NEGEDGE),
        TAG, "failed to configure H3 GPIO3 edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            SI_MS2109_H3_EVEN_GPIO2, on_h3_gpio2_falling_edge, &s_eeprom),
        TAG, "failed to install H3 GPIO2 edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            SI_MS2109_H3_EVEN_GPIO3, on_h3_gpio3_falling_edge, &s_eeprom),
        TAG, "failed to install H3 GPIO3 edge counter");

    return ESP_OK;
}
#endif

esp_err_t si_ms2109_eeprom_emulator_init(void)
{
#if !SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    memset(s_eeprom.image, 0xff, sizeof(s_eeprom.image));
    memcpy(s_eeprom.image, s_minimal_test_prefix, sizeof(s_minimal_test_prefix));

    i2c_slave_config_t config = {
        .i2c_port = -1,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .scl_io_num = SI_CFG_MS2109_EEPROM_SCL_GPIO,
        .sda_io_num = SI_CFG_MS2109_EEPROM_SDA_GPIO,
        .slave_addr = SI_MS2109_EEPROM_ADDRESS,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .send_buf_depth = SI_MS2109_EEPROM_TX_DEPTH,
        .receive_buf_depth = SI_MS2109_EEPROM_RX_DEPTH,
        .flags.enable_internal_pullup = false,
    };

    ESP_RETURN_ON_ERROR(i2c_new_slave_device(&config, &s_eeprom.handle), TAG,
                        "failed to create I2C slave");

    /*
     * ESP32-P4 has an autonomous slave-RAM addressing mode: the byte after
     * the slave address selects an offset and a following read is served
     * directly from the 32-byte TX RAM. The SCL edge ISR below turns that RAM
     * into a sliding window over the full 256-byte emulated address space.
     * This keeps the first byte autonomous and avoids address-match latency.
     */
    i2c_slave_dev_t *device = (i2c_slave_dev_t *)s_eeprom.handle;
    i2c_hal_context_t *hal = &device->base->hal;
    portENTER_CRITICAL(&device->base->spinlock);
    i2c_ll_disable_intr_mask(hal->dev, I2C_LL_INTR_MASK);
    i2c_ll_clear_intr_mask(hal->dev, I2C_LL_INTR_MASK);
    i2c_ll_enable_fifo_mode(hal->dev, false);
    i2c_ll_slave_enable_dual_addressing_mode(hal->dev, true);
    i2c_ll_slave_enable_scl_stretch(hal->dev, false);
    i2c_ll_txfifo_rst(hal->dev);
    i2c_ll_rxfifo_rst(hal->dev);
    i2c_ll_write_tx_by_nonfifo(
        hal->dev, 0, s_eeprom.image, SOC_I2C_FIFO_LEN);
    i2c_ll_slave_clear_stretch(hal->dev);
    i2c_ll_update(hal->dev);
    portEXIT_CRITICAL(&device->base->spinlock);
    s_eeprom.hardware_pointer = 0;
    s_eeprom.hardware_fifo_raddr = 0;
    s_eeprom.stream_pointer = 0;
    s_eeprom.hardware_pointer_valid = true;
    s_eeprom.hardware_read = false;

    ESP_RETURN_ON_ERROR(install_bus_edge_counters(), TAG,
                        "failed to install bus edge counters");

    ESP_LOGI(TAG,
             "virtual 24C02 ready: addr=0x%02x SCL=GPIO%d SDA=GPIO%d size=%u window=%u",
             SI_MS2109_EEPROM_ADDRESS,
             SI_CFG_MS2109_EEPROM_SCL_GPIO,
             SI_CFG_MS2109_EEPROM_SDA_GPIO,
             SI_MS2109_EEPROM_SIZE,
             SOC_I2C_FIFO_LEN);
    return ESP_OK;
#endif
}

void si_ms2109_eeprom_emulator_get_status(
    si_ms2109_eeprom_emulator_status_t *status)
{
    if (!status) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->enabled = SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED;
    status->scl_gpio = SI_CFG_MS2109_EEPROM_SCL_GPIO;
    status->sda_gpio = SI_CFG_MS2109_EEPROM_SDA_GPIO;

#if SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    status->initialized = s_eeprom.handle != NULL;
    portENTER_CRITICAL(&s_eeprom.lock);
    status->pointer = s_eeprom.pointer;
    status->scl_falling_edges = s_eeprom.scl_falling_edges;
    status->sda_falling_edges = s_eeprom.sda_falling_edges;
    status->h3_gpio2_falling_edges = s_eeprom.h3_gpio2_falling_edges;
    status->h3_gpio3_falling_edges = s_eeprom.h3_gpio3_falling_edges;
    status->receive_count = s_eeprom.receive_count;
    status->request_count = s_eeprom.request_count;
    portEXIT_CRITICAL(&s_eeprom.lock);

    if (GPIO_IS_VALID_GPIO(status->scl_gpio)) {
        status->scl_level = gpio_get_level(status->scl_gpio);
    }
    if (GPIO_IS_VALID_GPIO(status->sda_gpio)) {
        status->sda_level = gpio_get_level(status->sda_gpio);
    }
    status->h3_gpio2_level = gpio_get_level(SI_MS2109_H3_EVEN_GPIO2);
    status->h3_gpio3_level = gpio_get_level(SI_MS2109_H3_EVEN_GPIO3);
#endif
}

esp_err_t si_ms2109_eeprom_emulator_pull_line_low(bool scl)
{
#if !SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    (void)scl;
    return ESP_ERR_NOT_SUPPORTED;
#else
    const gpio_num_t gpio = scl ?
        SI_CFG_MS2109_EEPROM_SCL_GPIO : SI_CFG_MS2109_EEPROM_SDA_GPIO;
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(gpio), ESP_ERR_INVALID_ARG, TAG,
                        "invalid continuity-test GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, 0), TAG,
                        "failed to set continuity-test level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT_OD), TAG,
                        "failed to enable continuity-test open drain");
    ESP_LOGW(TAG, "continuity test: GPIO%d (%s) held low until reboot",
             gpio, scl ? "SCL" : "SDA");
    return ESP_OK;
#endif
}

esp_err_t si_ms2109_eeprom_emulator_test_external_pullups(
    int *scl_level, int *sda_level)
{
#if !SI_CFG_MS2109_EEPROM_EMULATOR_ENABLED
    (void)scl_level;
    (void)sda_level;
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(scl_level && sda_level, ESP_ERR_INVALID_ARG, TAG,
                        "missing pull-up test result pointer");

    ESP_RETURN_ON_ERROR(
        gpio_pulldown_en(SI_CFG_MS2109_EEPROM_SCL_GPIO), TAG,
        "failed to enable SCL weak pull-down");
    esp_err_t err = gpio_pulldown_en(SI_CFG_MS2109_EEPROM_SDA_GPIO);
    if (err != ESP_OK) {
        gpio_pulldown_dis(SI_CFG_MS2109_EEPROM_SCL_GPIO);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    *scl_level = gpio_get_level(SI_CFG_MS2109_EEPROM_SCL_GPIO);
    *sda_level = gpio_get_level(SI_CFG_MS2109_EEPROM_SDA_GPIO);

    esp_err_t scl_release =
        gpio_pulldown_dis(SI_CFG_MS2109_EEPROM_SCL_GPIO);
    esp_err_t sda_release =
        gpio_pulldown_dis(SI_CFG_MS2109_EEPROM_SDA_GPIO);
    if (scl_release != ESP_OK) {
        return scl_release;
    }
    return sda_release;
#endif
}
