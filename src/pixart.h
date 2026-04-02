#pragma once

/**
 * @file pixart.h
 *
 * @brief Common header file for all optical motion sensor by PIXART
 */

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input mode driven by active layer */
enum pixart_input_mode { MOVE = 0, SCROLL, SNIPE };

/* device data structure */
struct pixart_data {
    const struct device          *dev;
    bool                         sw_smart_flag; // for pmw3610 smart algorithm

    enum pixart_input_mode       curr_mode;  // current input mode (MOVE/SCROLL/SNIPE)
    uint32_t                     curr_cpi;   // current CPI (to avoid redundant writes)

    struct gpio_callback         irq_gpio_cb; // motion pin irq callback
    struct k_work                trigger_work; // real trigger job

    struct k_work_delayable      init_work; // the work structure for delayable init steps
    int                          async_init_step;

    bool                         ready; // whether init is finished successfully
    int                          err; // error code during async init
};

// device config data structure
struct pixart_config {
	struct spi_dt_spec spi;
    struct gpio_dt_spec irq_gpio;
    uint16_t cpi;       // normal mode CPI
    uint16_t snipe_cpi; // snipe mode CPI (0 = disabled)
    bool swap_xy;       // hardware swap X/Y (applied at sensor level, before rotation)
    bool inv_x;         // hardware invert X (applied at sensor level, before rotation)
    bool inv_y;         // hardware invert Y (applied at sensor level, before rotation)
    uint8_t evt_type;
    uint8_t x_input_code;
    uint8_t y_input_code;
    bool force_awake;
    bool force_awake_4ms_mode;
    // Layer-based orientation and mode switching
    size_t scroll_layers_len;
    int32_t *scroll_layers;
    size_t snipe_layers_len;
    int32_t *snipe_layers;
    uint8_t default_orientation_layer; // initial orientation layer activated at boot
};

#ifdef __cplusplus
}
#endif

/**
 * @}
 */
