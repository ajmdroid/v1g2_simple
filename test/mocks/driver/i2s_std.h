// Mock driver/i2s_std.h for native unit testing
#pragma once

#include <cstdint>
#include "gpio.h"

typedef int i2s_chan_handle_t;
typedef int i2s_port_t;
typedef int i2s_data_bit_width_t;
typedef int i2s_slot_bit_width_t;
typedef int i2s_slot_mode_t;
typedef int i2s_comm_format_t;
typedef int i2s_clock_src_t;

#define I2S_NUM_0           0
#define I2S_NUM_1           1
#define I2S_DATA_BIT_WIDTH_16BIT  16
#define I2S_DATA_BIT_WIDTH_32BIT  32
#define I2S_SLOT_BIT_WIDTH_AUTO   0
#define I2S_SLOT_MODE_STEREO      2
#define I2S_SLOT_MODE_MONO        1
#define I2S_CLK_SRC_DEFAULT       0

typedef enum {
    I2S_ROLE_MASTER = 0,
    I2S_ROLE_SLAVE  = 1,
} i2s_role_t;

#ifndef ESP_OK
#define ESP_OK 0
typedef int esp_err_t;
#endif

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    int dma_desc_num;
    int dma_frame_num;
    bool auto_clear;
} i2s_chan_config_t;

typedef struct {
    i2s_clock_src_t clk_src;
    int mclk_multiple;
    uint32_t sample_rate_hz;
} i2s_std_clk_config_t;

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
} i2s_std_slot_config_t;

typedef struct {
    bool mclk_inv;
    bool bclk_inv;
    bool ws_inv;
} i2s_std_gpio_invert_t;

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    i2s_std_gpio_invert_t invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t  clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { I2S_CLK_SRC_DEFAULT, 0, (uint32_t)(rate) }

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
    { (i2s_data_bit_width_t)(bits), I2S_SLOT_BIT_WIDTH_AUTO, (i2s_slot_mode_t)(mode) }

#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) \
    { (i2s_port_t)(port), (i2s_role_t)(role), 6, 240, false }

inline esp_err_t i2s_new_channel(const i2s_chan_config_t*, i2s_chan_handle_t* tx, i2s_chan_handle_t* rx) {
    if (tx) *tx = 1;
    if (rx) *rx = 1;
    return ESP_OK;
}
inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t*) { return ESP_OK; }
inline esp_err_t i2s_channel_enable(i2s_chan_handle_t) { return ESP_OK; }
inline esp_err_t i2s_channel_disable(i2s_chan_handle_t) { return ESP_OK; }
inline esp_err_t i2s_del_channel(i2s_chan_handle_t) { return ESP_OK; }
inline esp_err_t i2s_channel_write(i2s_chan_handle_t, const void*, size_t, size_t* written, uint32_t) {
    if (written) *written = 0;
    return ESP_OK;
}
