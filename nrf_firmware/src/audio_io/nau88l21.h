#pragma once

#include <stdint.h>

struct device;

#define NAU88L21_I2S_CODEC_CLOCK_MASTER 1

int nau88l21_init(const struct device *i2c_dev);
int nau88l21_set_fll_target_rate_hz(int32_t target_rate_hz);
