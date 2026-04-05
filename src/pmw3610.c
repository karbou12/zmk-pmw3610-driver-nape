/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pixart_pmw3610_alt

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device.h>
#include <zmk/keymap.h>
#include <zmk/events/activity_state_changed.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.1415926536
#endif
#include "pmw3610.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pmw3610, CONFIG_PMW3610_ALT_LOG_LEVEL);

//////// Sensor initialization steps definition //////////
// init is done in non-blocking manner (i.e., async), a //
// delayable work is defined for this purpose           //
enum pmw3610_init_step {
    ASYNC_INIT_STEP_POWER_UP,  // reset cs line and assert power-up reset
    ASYNC_INIT_STEP_CLEAR_OB1, // clear observation1 register for self-test check
    ASYNC_INIT_STEP_CHECK_OB1, // check the value of observation1 register after self-test check
    ASYNC_INIT_STEP_CONFIGURE, // set other registes like cpi and donwshift time (run, rest1, rest2)
                               // and clear motion registers

    ASYNC_INIT_STEP_COUNT // end flag
};

/* Timings (in ms) needed in between steps to allow each step finishes succussfully. */
// - Since MCU is not involved in the sensor init process, i is allowed to do other tasks.
//   Thus, k_sleep or delayed schedule can be used.
static const int32_t async_init_delay[ASYNC_INIT_STEP_COUNT] = {
    [ASYNC_INIT_STEP_POWER_UP] = 10 + CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS, // >10ms needed
    [ASYNC_INIT_STEP_CLEAR_OB1] = 200, // 150 us required, test shows too short,
                                       // also power-up reset is added in this step, thus using 50 ms
    [ASYNC_INIT_STEP_CHECK_OB1] = 50,  // 10 ms required in spec,
                                       // test shows too short,
                                       // especially when integrated with display,
                                       // > 50ms is needed
    [ASYNC_INIT_STEP_CONFIGURE] = 0,
};

static int pmw3610_async_init_power_up(const struct device *dev);
static int pmw3610_async_init_clear_ob1(const struct device *dev);
static int pmw3610_async_init_check_ob1(const struct device *dev);
static int pmw3610_async_init_configure(const struct device *dev);

static int (*const async_init_fn[ASYNC_INIT_STEP_COUNT])(const struct device *dev) = {
    [ASYNC_INIT_STEP_POWER_UP] = pmw3610_async_init_power_up,
    [ASYNC_INIT_STEP_CLEAR_OB1] = pmw3610_async_init_clear_ob1,
    [ASYNC_INIT_STEP_CHECK_OB1] = pmw3610_async_init_check_ob1,
    [ASYNC_INIT_STEP_CONFIGURE] = pmw3610_async_init_configure,
};

//////// Function definitions //////////

static int pmw3610_read(const struct device *dev, uint8_t addr, uint8_t *value, uint8_t len) {
	const struct pixart_config *cfg = dev->config;
	const struct spi_buf tx_buf = { .buf = &addr, .len = sizeof(addr) };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_buf[] = {
		{ .buf = NULL, .len = sizeof(addr), },
		{ .buf = value, .len = len, },
	};
	const struct spi_buf_set rx = { .buffers = rx_buf, .count = ARRAY_SIZE(rx_buf) };
	return spi_transceive_dt(&cfg->spi, &tx, &rx);
}

static int pmw3610_read_reg(const struct device *dev, uint8_t addr, uint8_t *value) {
	return pmw3610_read(dev, addr, value, 1);
}

static int pmw3610_write_reg(const struct device *dev, uint8_t addr, uint8_t value) {
	const struct pixart_config *cfg = dev->config;
	uint8_t write_buf[] = {addr | SPI_WRITE_BIT, value};
	const struct spi_buf tx_buf = { .buf = write_buf, .len = sizeof(write_buf), };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1, };
	return spi_write_dt(&cfg->spi, &tx);
}

static int pmw3610_write(const struct device *dev, uint8_t reg, uint8_t val) {
	pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
	k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    int err = pmw3610_write_reg(dev, reg, val);
    if (unlikely(err != 0)) {
        return err;
    }

    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);
    return 0;
}

static int pmw3610_set_cpi(const struct device *dev, uint32_t cpi,
                           bool swap_xy, bool inv_x, bool inv_y) {
    /* Set resolution with CPI step of 200 cpi
     * 0x1: 200 cpi (minimum cpi)
     * 0x2: 400 cpi
     * 0x3: 600 cpi
     * :
     */

    if ((cpi > PMW3610_MAX_CPI) || (cpi < PMW3610_MIN_CPI)) {
        LOG_ERR("CPI value %u out of range", cpi);
        return -EINVAL;
    }

    uint8_t value = 0x00;
    int err = 0;

    LOG_INF("Setting cpi: %d", cpi);
    // Convert CPI to register value
    // Set prefered RES_STEP
    //   BIT 4-0: CPI
    uint8_t cpi_val = cpi / 200;
    value = (value & 0xE0) | (cpi_val & 0x1F);

    // Hardware axis configuration (applied at sensor level, before software rotation)
    //   BIT 7: SWAP_XY
    //   BIT 6: INV_X
    //   BIT 5: INV_Y
    // Note: CONFIG_PMW3610_ALT_SWAP_XY/INVERT_X/Y apply SOFTWARE transforms after rotation.
    //       Use DTS swap-xy/invert-x/invert-y for hardware-level pre-rotation inversion.
    LOG_INF("Hardware axis: swap_xy=%s inv_x=%s inv_y=%s",
            swap_xy ? "yes" : "no", inv_x ? "yes" : "no", inv_y ? "yes" : "no");
    if (swap_xy) { value |= (1 << 7); } else { value &= ~(1 << 7); }
    if (inv_x)   { value |= (1 << 6); } else { value &= ~(1 << 6); }
    if (inv_y)   { value |= (1 << 5); } else { value &= ~(1 << 5); }

    LOG_INF("Setting CPI to %u (reg value 0x%x)", cpi, value);

    /* set the cpi */
    uint8_t addr[] = {0x7F, PMW3610_REG_RES_STEP, 0x7F};
    uint8_t data[] = {0xFF, value,                0x00};

	pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_ENABLE);
	k_sleep(K_USEC(T_CLOCK_ON_DELAY_US));

    /* Write data */
    for (size_t i = 0; i < sizeof(data); i++) {
        err = pmw3610_write_reg(dev, addr[i], data[i]);
        if (err) {
            LOG_ERR("Burst write failed on SPI write (data)");
            break;
        }
    }
    pmw3610_write_reg(dev, PMW3610_REG_SPI_CLK_ON_REQ, PMW3610_SPI_CLOCK_CMD_DISABLE);

    if (err) {
        LOG_ERR("Failed to set CPI");
        return err;
    }

    return 0;
}

/* Set sampling rate in each mode (in ms) */
static int pmw3610_set_sample_time(const struct device *dev, uint8_t reg_addr, uint32_t sample_time) {
    uint32_t maxtime = 2550;
    uint32_t mintime = 10;
    if ((sample_time > maxtime) || (sample_time < mintime)) {
        LOG_WRN("Sample time %u out of range [%u, %u]", sample_time, mintime, maxtime);
        return -EINVAL;
    }

    uint8_t value = sample_time / mintime;
    LOG_INF("Set sample time to %u ms (reg value: 0x%x)", sample_time, value);

    /* The sample time is (reg_value * mintime ) ms. 0x00 is rounded to 0x1 */
    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change sample time");
    }

    return err;
}

/* Set downshift time in ms. */
// NOTE: The unit of run-mode downshift is related to pos mode rate, which is hard coded to be 4 ms
// The pos-mode rate is configured in pmw3610_async_init_configure
static int pmw3610_set_downshift_time(const struct device *dev, uint8_t reg_addr, uint32_t time) {
    uint32_t maxtime;
    uint32_t mintime;

    switch (reg_addr) {
    case PMW3610_REG_RUN_DOWNSHIFT:
        /*
         * Run downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                      * 8 * pos-rate (fixed to 4ms)
         */
        maxtime = 8160; // 32 * 255;
        mintime = 32; // hard-coded in pmw3610_async_init_configure
        break;

    case PMW3610_REG_REST1_DOWNSHIFT:
        /*
         * Rest1 downshift time = PMW3610_REG_RUN_DOWNSHIFT
         *                        * 16 * Rest1_sample_period (default 40 ms)
         */
        maxtime = 255 * 16 * CONFIG_PMW3610_ALT_REST1_SAMPLE_TIME_MS;
        mintime = 16 * CONFIG_PMW3610_ALT_REST1_SAMPLE_TIME_MS;
        break;

    case PMW3610_REG_REST2_DOWNSHIFT:
        /*
         * Rest2 downshift time = PMW3610_REG_REST2_DOWNSHIFT
         *                        * 128 * Rest2 rate (default 100 ms)
         */
        maxtime = 255 * 128 * CONFIG_PMW3610_ALT_REST2_SAMPLE_TIME_MS;
        mintime = 128 * CONFIG_PMW3610_ALT_REST2_SAMPLE_TIME_MS;
        break;

    default:
        LOG_ERR("Not supported");
        return -ENOTSUP;
    }

    if ((time > maxtime) || (time < mintime)) {
        LOG_WRN("Downshift time %u out of range (%u - %u)", time, mintime, maxtime);
        return -EINVAL;
    }

    __ASSERT_NO_MSG((mintime > 0) && (maxtime / mintime <= UINT8_MAX));

    /* Convert time to register value */
    uint8_t value = time / mintime;

    LOG_INF("Set downshift time to %u ms (reg value 0x%x)", time, value);

    int err = pmw3610_write(dev, reg_addr, value);
    if (err) {
        LOG_ERR("Failed to change downshift time");
    }

    return err;
}

static int pmw3610_set_performance(const struct device *dev, bool enabled) {
    const struct pixart_config *config = dev->config;
    int err = 0;

    if (config->force_awake) {
        uint8_t value;
        err = pmw3610_read_reg(dev, PMW3610_REG_PERFORMANCE, &value);
        if (err) {
            LOG_ERR("Can't read ref-performance %d", err);
            return err;
        }
        LOG_INF("Get performance register (reg value 0x%x)", value);

        // Set prefered RUN RATE
        //   BIT 3:   VEL_RUNRATE    0x0: 8ms; 0x1 4ms;
        //   BIT 2:   POSHI_RUN_RATE 0x0: 8ms; 0x1 4ms;
        //   BIT 1-0: POSLO_RUN_RATE 0x0: 8ms; 0x1 4ms; 0x2 2ms; 0x4 Reserved
        uint8_t perf;
        if (config->force_awake_4ms_mode) {
            perf = 0x0d; // RUN RATE @ 4ms
        } else {
            // reset bit[3..0] to 0x0 (normal operation)
            perf = value & 0x0F; // RUN RATE @ 8ms
        }

        if (enabled) {
            perf |= 0xF0; // set bit[3..0] to 0xF (force awake)
        }
        if (perf != value) {
            err = pmw3610_write(dev, PMW3610_REG_PERFORMANCE, perf);
            if (err) {
                LOG_ERR("Can't write performance register %d", err);
                return err;
            }
            LOG_INF("Set performance register (reg value 0x%x)", perf);
        }
        LOG_INF("%s performance mode", enabled ? "enable" : "disable");
    }

    return err;
}

static int pmw3610_set_interrupt(const struct device *dev, const bool en) {
    const struct pixart_config *config = dev->config;
    int ret = gpio_pin_interrupt_configure_dt(&config->irq_gpio,
                                              en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
    return ret;
}

static int pmw3610_async_init_power_up(const struct device *dev) {
	int ret = pmw3610_write_reg(dev, PMW3610_REG_POWER_UP_RESET, PMW3610_POWERUP_CMD_RESET);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static int pmw3610_async_init_clear_ob1(const struct device *dev) {
    return pmw3610_write(dev, PMW3610_REG_OBSERVATION, 0x00);
}

static int pmw3610_async_init_check_ob1(const struct device *dev) {
    uint8_t value;
    int err = pmw3610_read_reg(dev, PMW3610_REG_OBSERVATION, &value);
    if (err) {
        LOG_ERR("Can't do self-test");
        return err;
    }

    if ((value & 0x0F) != 0x0F) {
        LOG_ERR("Failed self-test (0x%x)", value);
        return -EINVAL;
    }

    uint8_t product_id = 0x01;
    err = pmw3610_read_reg(dev, PMW3610_REG_PRODUCT_ID, &product_id);
    if (err) {
        LOG_ERR("Cannot obtain product id");
        return err;
    }

    if (product_id != PMW3610_PRODUCT_ID) {
        LOG_ERR("Incorrect product id 0x%x (expecting 0x%x)!", product_id, PMW3610_PRODUCT_ID);
        return -EIO;
    }

    return 0;
}

static int pmw3610_async_init_configure(const struct device *dev) {
    int err = 0;
    const struct pixart_config *config = dev->config;

    // clear motion registers first (required in datasheet)
    for (uint8_t reg = 0x02; (reg <= 0x05) && !err; reg++) {
        uint8_t buf[1];
        err = pmw3610_read_reg(dev, reg, buf);
    }

    if (!err) {
        err = pmw3610_set_performance(dev, true);
    }

    if (!err) {
        err = pmw3610_set_cpi(dev, config->cpi, config->swap_xy, config->inv_x, config->inv_y);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT,
                                         CONFIG_PMW3610_ALT_RUN_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT,
                                         CONFIG_PMW3610_ALT_REST1_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT,
                                         CONFIG_PMW3610_ALT_REST2_DOWNSHIFT_TIME_MS);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST1_RATE,
                                      CONFIG_PMW3610_ALT_REST1_SAMPLE_TIME_MS);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST2_RATE,
                                      CONFIG_PMW3610_ALT_REST2_SAMPLE_TIME_MS);
    }

    if (!err) {
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST3_RATE,
                                      CONFIG_PMW3610_ALT_REST3_SAMPLE_TIME_MS);
    }

    if (err) {
        LOG_ERR("Config the sensor failed");
        return err;
    }

    return 0;
}

static void pmw3610_async_init(struct k_work *work) {
    struct k_work_delayable *work2 = (struct k_work_delayable *)work;
    struct pixart_data *data = CONTAINER_OF(work2, struct pixart_data, init_work);
    const struct device *dev = data->dev;

    LOG_INF("PMW3610 async init step %d", data->async_init_step);

    data->err = async_init_fn[data->async_init_step](dev);
    if (data->err) {
        LOG_ERR("PMW3610 initialization failed in step %d", data->async_init_step);
    } else {
        data->async_init_step++;

        if (data->async_init_step == ASYNC_INIT_STEP_COUNT) {
            data->ready = true; // sensor is ready to work
            LOG_INF("PMW3610 initialized");
            pmw3610_set_interrupt(dev, true);
        } else {
            k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
        }
    }
}

//////// Layer-based orientation and mode detection //////////

// Tracks the orientation layer last active during MOVE mode.
// Updated only when in MOVE mode so that scroll/snipe layers don't change the orientation.
static uint8_t last_orientation_layer = 0;

#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT

static int8_t prev_direction = -1;
static uint8_t direction_degree = 0;

static uint16_t calc_degree_for_direction() {
    return (prev_direction == -1) ? last_orientation_layer * 45 : prev_direction * direction_degree;
}

static uint16_t calc_degree_in_range(const int16_t degree) {
    const int16_t mod = degree % 360;
    return (mod >= 0) ? mod : mod + 360;
}

static int8_t rotate_device(const bool is_cw) {
    const uint8_t max_direction = 360 / direction_degree;
    const uint8_t cur_direction = (prev_direction != -1) ? prev_direction
        : (uint8_t)(last_orientation_layer * (max_direction / 8.0));
    const int8_t next_direction = is_cw ? cur_direction + 1 : cur_direction - 1;

    prev_direction = (next_direction < 0) ? max_direction - 1
        : (max_direction <= next_direction) ? 0
        : next_direction;
    LOG_INF("rotate direction %d -> %d", cur_direction, prev_direction);
    return prev_direction;
}

uint8_t calc_step_angle_degree_in_range(const uint8_t step_angle_degree) {
    const uint8_t min_angle = 3;
    const uint8_t max_angle = 45;
    uint8_t calc_angle = step_angle_degree;

    if (calc_angle < min_angle) {
        calc_angle = min_angle;
    } else if (max_angle < calc_angle) {
        calc_angle = max_angle;
    }

    while (360 % calc_angle != 0) {
        calc_angle++;
    }

    if (step_angle_degree != calc_angle) {
        LOG_WRN("step angle is changed from %u to %u [degrees].", step_angle_degree, calc_angle);
    }

    return calc_angle;
}

void rotate_device_with_step(const int8_t step_angle_degree) {
    static int8_t sum_angle_degree = 0;
    static int8_t total = 0;

    static int64_t prev_time = 0;
    int64_t curr_time = k_uptime_get();
    const int64_t diff_time = curr_time - prev_time;
    if ((prev_time == 0) || (diff_time > 3000)) {
        prev_time = curr_time;
        sum_angle_degree = 0;
        total = 0;
    }

    sum_angle_degree += step_angle_degree;
    LOG_DBG("%s %d -> %d\n", __FUNCTION__, step_angle_degree, sum_angle_degree);

    bool is_rotate = false;
    uint8_t count = 0;
    int8_t tmp_angle_degree = sum_angle_degree;

    if (sum_angle_degree > 0) {
        // int8_t tmp_angle_degree = sum_angle_degree + direction_degree / 2;
        while (tmp_angle_degree >= direction_degree) {
            rotate_device(true);
            is_rotate = true;
            count++;
            total++;
            tmp_angle_degree -= direction_degree;
            if (tmp_angle_degree < 0) {
                tmp_angle_degree = 0;
            }
        }
    } else if (sum_angle_degree < 0) {
        // int8_t tmp_angle_degree = sum_angle_degree - direction_degree / 2;
        while (tmp_angle_degree <= -direction_degree) {
            rotate_device(false);
            is_rotate = true;
            count++;
            total++;
            tmp_angle_degree += direction_degree;
            if (tmp_angle_degree > 0) {
                tmp_angle_degree = 0;
            }
        }
    }

    if (is_rotate) {
        LOG_DBG("count:%d, total:%d, remain angle:%d\n", count, total, tmp_angle_degree);
        sum_angle_degree = tmp_angle_degree;
        return;
    }
}

static int8_t detect_direction(const int16_t cur_x, const int16_t cur_y) {
    static const uint32_t dir_detect_threshold = CONFIG_PMW3610_ALT_TRABO_SHIFT_DISTANCE_THRESHOLD * CONFIG_PMW3610_ALT_TRABO_SHIFT_DISTANCE_THRESHOLD;

    static int16_t x = 0;
    static int16_t y = 0;
    static int64_t prev_time = 0;

    int64_t curr_time = k_uptime_get();

    const int64_t diff_time = curr_time - prev_time;
    if ((prev_time == 0) || (diff_time > CONFIG_PMW3610_ALT_TRABO_SHIFT_SAMPLE_TIME_MS * 2)) {
        LOG_DBG("detection begin at %lld", curr_time);
        prev_time = curr_time;
        x = cur_x;
        y = cur_y;
        return prev_direction;
    }

    x += cur_x;
    y += cur_y;

    const uint32_t distance = x * x + y * y;

    if (diff_time < CONFIG_PMW3610_ALT_TRABO_SHIFT_SAMPLE_TIME_MS) {
        LOG_DBG("under detection [dst:%d %d -> %u/%u] [time:%lld - %lld = %lld/%d]",
                x, y, distance, dir_detect_threshold,
                curr_time, prev_time, diff_time, CONFIG_PMW3610_ALT_TRABO_SHIFT_SAMPLE_TIME_MS);

        if (distance < dir_detect_threshold) {
            return prev_direction;
        }
    }

    const double radian = atan2(y, x);
    int16_t degree = (int16_t)(radian * 180 / M_PI);

    LOG_DBG("finish detection [dst:%d %d (degree:%d) -> %u/%u] [time:%lld - %lld = %lld/%d]",
            x, y, degree, distance, dir_detect_threshold,
            curr_time, prev_time, diff_time, CONFIG_PMW3610_ALT_TRABO_SHIFT_SAMPLE_TIME_MS);

    prev_time = 0;
    x = 0;
    y = 0;

    if (distance < dir_detect_threshold) {
        return prev_direction;
    }

    LOG_INF("********** change direction");

    degree -= 270;
    degree += (int16_t)(direction_degree / 2);
    degree = calc_degree_in_range(degree);
    LOG_DBG("corrected degree:%d", degree);

    return (int8_t)(degree / direction_degree);
}

static void rotate_point(const int16_t raw_x, const int16_t raw_y, const uint16_t degree, int16_t* x, int16_t* y) {
    // normalize sin table with int16_t max
    static const int32_t sin_tbl[] = {
            0,   572,  1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
         5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580, 10126, 10668,
        11207, 11743, 12275, 12803, 13328, 13848, 14364, 14876, 15383, 15886,
        16383, 16876, 17364, 17846, 18323, 18794, 19260, 19720, 20173, 20621,
        21062, 21497, 21925, 22347, 22762, 23170, 23571, 23964, 24351, 24730,
        25101, 25465, 25821, 26169, 26509, 26841, 27165, 27481, 27788, 28087,
        28377, 28659, 28932, 29196, 29451, 29697, 29934, 30162, 30381, 30591,
        30791, 30982, 31163, 31335, 31498, 31650, 31794, 31927, 32051, 32165,
        32269, 32364, 32448, 32523, 32587, 32642, 32687, 32722, 32747, 32762,
        32767,
    };
    const int32_t SCALER = 32767;

    if (degree < 90) {
        const uint8_t sin_index = degree;
        const uint8_t cos_index  = 90 - degree;
        *x = (int16_t)((  raw_x * sin_tbl[cos_index] + raw_y * sin_tbl[sin_index]) / SCALER);
        *y = (int16_t)((- raw_x * sin_tbl[sin_index] + raw_y * sin_tbl[cos_index]) / SCALER);
    } else if (degree < 180) {
        const uint8_t sin_index = 180 - degree;
        const uint8_t cos_index  = degree - 90;
        *x = (int16_t)((- raw_x * sin_tbl[cos_index] + raw_y * sin_tbl[sin_index]) / SCALER);
        *y = (int16_t)((- raw_x * sin_tbl[sin_index] - raw_y * sin_tbl[cos_index]) / SCALER);
    } else if (degree < 270) {
        const uint8_t sin_index = degree - 180;
        const uint8_t cos_index  = 270 - degree;
        *x = (int16_t)((- raw_x * sin_tbl[cos_index] - raw_y * sin_tbl[sin_index]) / SCALER);
        *y = (int16_t)((  raw_x * sin_tbl[sin_index] - raw_y * sin_tbl[cos_index]) / SCALER);
    } else  {
        const uint8_t sin_index = 360 - degree;
        const uint8_t cos_index  = degree - 270;
        *x = (int16_t)((  raw_x * sin_tbl[cos_index] - raw_y * sin_tbl[sin_index]) / SCALER);
        *y = (int16_t)((  raw_x * sin_tbl[sin_index] + raw_y * sin_tbl[cos_index]) / SCALER);
    }
}
#endif

static enum pixart_input_mode get_input_mode_for_current_layer(const struct device *dev) {
    const struct pixart_config *config = dev->config;
    uint8_t curr_layer = zmk_keymap_highest_layer_active();
    for (size_t i = 0; i < config->scroll_layers_len; i++) {
        if (curr_layer == (uint8_t)config->scroll_layers[i]) {
            return SCROLL;
        }
    }
    for (size_t i = 0; i < config->snipe_layers_len; i++) {
        if (curr_layer == (uint8_t)config->snipe_layers[i]) {
            return SNIPE;
        }
    }
    return MOVE;
}

//////// Motion reporting //////////

// 12-bit two's complement value to int16_t
// adapted from https://stackoverflow.com/questions/70802306/convert-a-12-bit-signed-number-in-c
#define TOINT16(val, bits) (((struct { int16_t value : bits; }){val}).value)

static int pmw3610_report_data(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    uint8_t buf[PMW3610_BURST_SIZE];

    if (unlikely(!data->ready)) {
        LOG_WRN("Device is not initialized yet");
        return -EBUSY;
    }

    // Determine input mode from active layer
    enum pixart_input_mode input_mode = get_input_mode_for_current_layer(dev);
    uint8_t current_layer = zmk_keymap_highest_layer_active();

    // Only update orientation when in MOVE mode; scroll/snipe layers preserve last orientation
    if (input_mode == MOVE) {
#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT
        if (current_layer != CONFIG_PMW3610_ALT_TRABO_SHIFT_LAYER) {
#endif
            last_orientation_layer = current_layer;
#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT
        }
#endif
    }

    // Switch CPI based on input mode
    uint32_t target_cpi = (input_mode == SNIPE && config->snipe_cpi > 0)
                          ? config->snipe_cpi
                          : config->cpi;
    if (data->curr_cpi != target_cpi) {
        pmw3610_set_cpi(dev, target_cpi, config->swap_xy, config->inv_x, config->inv_y);
        data->curr_cpi = target_cpi;
    }

    static int64_t dx = 0;
    static int64_t dy = 0;

#if CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN > 0
    static int64_t last_smp_time = 0;
    static int64_t last_rpt_time = 0;
    int64_t now = k_uptime_get();
#endif

	int err = pmw3610_read(dev, PMW3610_REG_MOTION_BURST, buf, PMW3610_BURST_SIZE);
    if (err) {
        return err;
    }

    int16_t raw_x = TOINT16((buf[PMW3610_X_L_POS] + ((buf[PMW3610_XY_H_POS] & 0xF0) << 4)), 12);
    int16_t raw_y = TOINT16((buf[PMW3610_Y_L_POS] + ((buf[PMW3610_XY_H_POS] & 0x0F) << 8)), 12);
    LOG_DBG("raw x/y: %d/%d layer: %d", raw_x, raw_y, last_orientation_layer);

#ifdef CONFIG_PMW3610_ALT_SMART_ALGORITHM
    int16_t shutter = ((int16_t)(buf[PMW3610_SHUTTER_H_POS] & 0x01) << 8)
                    + buf[PMW3610_SHUTTER_L_POS];
    if (data->sw_smart_flag && shutter < 45) {
        pmw3610_write(dev, 0x32, 0x00);
        data->sw_smart_flag = false;
    }
    if (!data->sw_smart_flag && shutter > 45) {
        pmw3610_write(dev, 0x32, 0x80);
        data->sw_smart_flag = true;
    }
#endif

#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT
    static bool is_direction_changed = false;

    if (input_mode == MOVE) {
        if (zmk_keymap_highest_layer_active() == CONFIG_PMW3610_ALT_TRABO_SHIFT_LAYER) {
            if (is_direction_changed) {
                LOG_DBG("skip to detect direction");
                return 0;
            }

            const int8_t detected_direction = detect_direction(raw_x, raw_y);
            if (prev_direction != detected_direction) {
                LOG_INF("direction %d -> %d", prev_direction, detected_direction);
                prev_direction = detected_direction;
                is_direction_changed = true;
            }

            return 0;
        } else if (is_direction_changed) {
            LOG_DBG("reset is_direction_changed");
            is_direction_changed = false;
        }
    }

    const uint16_t degree = calc_degree_for_direction();
#endif

    // Apply layer-based rotation transform.
    // Layer 0 = 0°, Layer 1 = 45°, ..., Layer 7 = 315°.
    // This allows the Nape to be held in 8 different orientations.
    int16_t x;
    int16_t y;

#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT
    rotate_point(raw_x, raw_y, degree, &x, &y);
#else
    switch (last_orientation_layer) {
    case 1: // 45°
        x = ((raw_x + raw_y) * 100) / 141;
        y = ((raw_y - raw_x) * 100) / 141;
        break;
    case 2: // 90°
        x = raw_y;
        y = -raw_x;
        break;
    case 3: // 135°
        x = ((raw_y - raw_x) * 100) / 141;
        y = -((raw_x + raw_y) * 100) / 141;
        break;
    case 4: // 180°
        x = -raw_x;
        y = -raw_y;
        break;
    case 5: // 225°
        x = -((raw_x + raw_y) * 100) / 141;
        y = -((raw_y - raw_x) * 100) / 141;
        break;
    case 6: // 270°
        x = -raw_y;
        y = raw_x;
        break;
    case 7: // 315°
        x = -((raw_y - raw_x) * 100) / 141;
        y = ((raw_x + raw_y) * 100) / 141;
        break;
    default: // 0° (layer 0 or any unrecognized layer)
        x = raw_x;
        y = raw_y;
        break;
    }
#endif

    // Software post-rotation axis inversion.
    // Applied after the rotation transform, unlike the hardware inv_x/inv_y (pre-rotation).
    // Use CONFIG_PMW3610_ALT_INVERT_X/Y in nape.conf to match physical wiring orientation.
    if (IS_ENABLED(CONFIG_PMW3610_ALT_INVERT_X)) {
        x = -x;
    }
    if (IS_ENABLED(CONFIG_PMW3610_ALT_INVERT_Y)) {
        y = -y;
    }

#if CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN > 0
    // purge accumulated delta, if last sampled had not been reported on last report tick
    if (now - last_smp_time >= CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN) {
        dx = 0;
        dy = 0;
    }
    last_smp_time = now;
#endif

    // accumulate delta until report in next iteration
    dx += x;
    dy += y;

#if CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN > 0
    // strict to report interval
    if (now - last_rpt_time < CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN) {
        return 0;
    }
#endif

    // fetch report value
    int16_t rx = (int16_t)CLAMP(dx, INT16_MIN, INT16_MAX);
    int16_t ry = (int16_t)CLAMP(dy, INT16_MIN, INT16_MAX);
    bool have_x = rx != 0;
    bool have_y = ry != 0;

    if (have_x || have_y) {
#if CONFIG_PMW3610_ALT_REPORT_INTERVAL_MIN > 0
        last_rpt_time = now;
#endif
        dx = 0;
        dy = 0;
        if (have_x) {
            input_report(dev, config->evt_type, config->x_input_code, rx, !have_y, K_NO_WAIT);
        }
        if (have_y) {
            input_report(dev, config->evt_type, config->y_input_code, ry, true, K_NO_WAIT);
        }
    }

    return err;
}

static void pmw3610_gpio_callback(const struct device *gpiob, struct gpio_callback *cb,
                                  uint32_t pins) {
    struct pixart_data *data = CONTAINER_OF(cb, struct pixart_data, irq_gpio_cb);
    const struct device *dev = data->dev;
    pmw3610_set_interrupt(dev, false);
    k_work_submit(&data->trigger_work);
}

static void pmw3610_work_callback(struct k_work *work) {
    struct pixart_data *data = CONTAINER_OF(work, struct pixart_data, trigger_work);
    const struct device *dev = data->dev;
    pmw3610_report_data(dev);
    pmw3610_set_interrupt(dev, true);
}

static int pmw3610_init_irq(const struct device *dev) {
    int err;
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;

    // check readiness of irq gpio pin
    if (!device_is_ready(config->irq_gpio.port)) {
        LOG_ERR("IRQ GPIO device not ready");
        return -ENODEV;
    }

    // init the irq pin
    err = gpio_pin_configure_dt(&config->irq_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Cannot configure IRQ GPIO");
        return err;
    }

    // setup and add the irq callback associated
    gpio_init_callback(&data->irq_gpio_cb, pmw3610_gpio_callback, BIT(config->irq_gpio.pin));

    err = gpio_add_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    if (err) {
        LOG_ERR("Cannot add IRQ GPIO callback");
    }

    return err;
}

static int pmw3610_init(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err;

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("%s is not ready", config->spi.bus->name);
		return -ENODEV;
	}

    // init device pointer
    data->dev = dev;

    // init smart algorithm flag
    data->sw_smart_flag = false;

    // init CPI tracking to normal CPI
    data->curr_cpi = config->cpi;

    // Initialize orientation layer and optionally activate it in the keymap.
    // This lets the keyboard boot into a known physical orientation.
    last_orientation_layer = config->default_orientation_layer;
    if (config->default_orientation_layer > 0) {
        LOG_INF("Activating default orientation layer %d", config->default_orientation_layer);
        zmk_keymap_layer_activate(config->default_orientation_layer, false);
    }

#ifdef CONFIG_PMW3610_ALT_TRABO_SHIFT
    direction_degree = calc_step_angle_degree_in_range(CONFIG_PMW3610_ALT_TRABO_SHIFT_ANGLE);
#endif

    // init trigger handler work
    k_work_init(&data->trigger_work, pmw3610_work_callback);

    // init irq routine
    err = pmw3610_init_irq(dev);
    if (err) {
        return err;
    }

    // Setup delayable and non-blocking init jobs, including following steps:
    // 1. power reset
    // 2. upload initial settings
    // 3. other configs like cpi, downshift time, sample time etc.
    // The sensor is ready to work (i.e., data->ready=true after the above steps are finished)
    k_work_init_delayable(&data->init_work, pmw3610_async_init);

    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));

    return err;
}

static int pmw3610_alt_attr_set(const struct device *dev, enum sensor_channel chan,
                            enum sensor_attribute attr, const struct sensor_value *val) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err;

    if (unlikely(chan != SENSOR_CHAN_ALL)) {
        return -ENOTSUP;
    }

    if (unlikely(!data->ready)) {
        LOG_DBG("Device is not initialized yet");
        return -EBUSY;
    }

    switch ((uint32_t)attr) {
    case PMW3610_ALT_ATTR_CPI:
        err = pmw3610_set_cpi(dev, PMW3610_SVALUE_TO_CPI(*val),
                              config->swap_xy, config->inv_x, config->inv_y);
        if (!err) {
            data->curr_cpi = PMW3610_SVALUE_TO_CPI(*val);
        }
        break;

    case PMW3610_ALT_ATTR_RUN_DOWNSHIFT_TIME:
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_RUN_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ALT_ATTR_REST1_DOWNSHIFT_TIME:
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST1_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ALT_ATTR_REST2_DOWNSHIFT_TIME:
        err = pmw3610_set_downshift_time(dev, PMW3610_REG_REST2_DOWNSHIFT, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ALT_ATTR_REST1_SAMPLE_TIME:
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST1_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ALT_ATTR_REST2_SAMPLE_TIME:
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST2_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    case PMW3610_ALT_ATTR_REST3_SAMPLE_TIME:
        err = pmw3610_set_sample_time(dev, PMW3610_REG_REST3_RATE, PMW3610_SVALUE_TO_TIME(*val));
        break;

    default:
        LOG_ERR("Unknown attribute");
        err = -ENOTSUP;
    }

    return err;
}

static const struct sensor_driver_api pmw3610_driver_api = {
    .attr_set = pmw3610_alt_attr_set,
};

#define PMW3610_SPI_MODE (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | \
                        SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#define PMW3610_DEFINE(n)                                                                          \
    static struct pixart_data data##n;                                                             \
    static int32_t scroll_layers##n[] = DT_PROP(DT_DRV_INST(n), scroll_layers);                   \
    static int32_t snipe_layers##n[]  = DT_PROP(DT_DRV_INST(n), snipe_layers);                    \
    static const struct pixart_config config##n = {                                                \
		.spi = SPI_DT_SPEC_INST_GET(n, PMW3610_SPI_MODE, 0),		                               \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .cpi = DT_PROP(DT_DRV_INST(n), cpi),                                                       \
        .snipe_cpi = DT_PROP(DT_DRV_INST(n), snipe_cpi),                                           \
        .swap_xy = DT_PROP(DT_DRV_INST(n), swap_xy),                                               \
        .inv_x = DT_PROP(DT_DRV_INST(n), invert_x),                                                \
        .inv_y = DT_PROP(DT_DRV_INST(n), invert_y),                                                \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
        .force_awake = DT_PROP(DT_DRV_INST(n), force_awake),                                       \
        .force_awake_4ms_mode = DT_PROP(DT_DRV_INST(n), force_awake_4ms_mode),                     \
        .scroll_layers = scroll_layers##n,                                                         \
        .scroll_layers_len = DT_PROP_LEN(DT_DRV_INST(n), scroll_layers),                           \
        .snipe_layers = snipe_layers##n,                                                           \
        .snipe_layers_len = DT_PROP_LEN(DT_DRV_INST(n), snipe_layers),                             \
        .default_orientation_layer = DT_PROP(DT_DRV_INST(n), default_orientation_layer),           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pmw3610_init, NULL, &data##n, &config##n, POST_KERNEL,                \
                          CONFIG_INPUT_PMW3610_INIT_PRIORITY, &pmw3610_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PMW3610_DEFINE)


#define GET_PMW3610_DEV(node_id) DEVICE_DT_GET(node_id),

static const struct device *pmw3610_devs[] = {
    DT_FOREACH_STATUS_OKAY(pixart_pmw3610_alt, GET_PMW3610_DEV)
};

static int on_activity_state(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *state_ev = as_zmk_activity_state_changed(eh);

    if (!state_ev) {
        LOG_WRN("NO EVENT, leaving early");
        return 0;
    }

    bool enable = state_ev->state == ZMK_ACTIVITY_ACTIVE ? 1 : 0;
    for (size_t i = 0; i < ARRAY_SIZE(pmw3610_devs); i++) {
        pmw3610_set_performance(pmw3610_devs[i], enable);
    }

    return 0;
}

ZMK_LISTENER(zmk_pmw3610_idle_sleeper, on_activity_state);
ZMK_SUBSCRIPTION(zmk_pmw3610_idle_sleeper, zmk_activity_state_changed);
