#pragma once

struct device;

#define NAU88L21_I2S_CODEC_CLOCK_MASTER 1

int nau88l21_init(const struct device *i2c_dev);
