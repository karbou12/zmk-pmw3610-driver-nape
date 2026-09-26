/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>
#include <stdint.h>

/* CPI is 200..3200 in steps of 200. The sensor applies it on the next motion. */
int nape_pmw3610_set_cpi(const struct device *dev, uint16_t cpi);
uint16_t nape_pmw3610_get_cpi(const struct device *dev);
uint16_t nape_pmw3610_stock_cpi(const struct device *dev);
/* A zero layer CPI inherits the normal CPI. IDs, not display order, are stored. */
int nape_pmw3610_set_layer_cpi(const struct device *dev, uint8_t layer_id, uint16_t cpi);
uint16_t nape_pmw3610_get_layer_cpi(const struct device *dev, uint8_t layer_id);
uint8_t nape_pmw3610_get_boot_layer(const struct device *dev);
uint8_t nape_pmw3610_stock_boot_layer(const struct device *dev);
int nape_pmw3610_set_boot_layer(const struct device *dev, uint8_t layer_id);
/* Called only during settings-load commit. Runtime edits take effect next boot. */
int nape_pmw3610_apply_boot_layer(const struct device *dev);
