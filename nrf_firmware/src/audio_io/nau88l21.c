#include "audio_io/nau88l21.h"

#include <stdint.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define NAU88L21_I2C_ADDR 0x1B

/* The datasheet prose and register table disagree on reset wording.
 * The register table places the reset register at 0x00, so use that as the
 * authoritative address for the initial defensive reset in bring-up.
 */
#define NAU88L21_REG_RESET                         0x00
#define NAU88L21_REG_I2S_PCM_CTRL1                 0x1C
#define NAU88L21_REG_I2S_PCM_CTRL2                 0x1D
#define NAU88L21_REG_DAC_CONTROL                   0x73
#define NAU88L21_REG_BOOST                         0x76
#define NAU88L21_REG_POWER_UP_CONTROL              0x7F
#define NAU88L21_REG_CHARGE_PUMP_CONTROL           0x80
#define NAU88L21_REG_GENERAL_STATUS                0x82

/* First-pass playback bring-up for the NAU88L21 eval board:
 * - standard I2S
 * - 16-bit stereo
 * - codec slave mode
 * - DAC/headphone path powered up
 *
 * These values are derived from the datasheet register tables and kept
 * intentionally minimal for the first audible-tone milestone.
 */
#define NAU88L21_I2S_PCM_CTRL1_I2S_16BIT           0x0002
#define NAU88L21_I2S_PCM_CTRL2_I2S_48K             0x301B
#define NAU88L21_DAC_CONTROL_ENABLE_BOTH           0x0F08
#define NAU88L21_BOOST_ENABLE_ANALOG_BIAS          0x0100
#define NAU88L21_POWER_UP_OUTPUT_INTEGRATORS       0x0030
#define NAU88L21_POWER_UP_DRIVER_STAGE             0x003C
#define NAU88L21_POWER_UP_DRIVER_MAIN              0x003F
#define NAU88L21_CHARGE_PUMP_ENABLE_PRECHARGE      0x0B30

static int nau88l21_write_reg(const struct device *i2c_dev, uint16_t reg, uint16_t value)
{
	uint8_t buf[4] = {
		(uint8_t)(reg >> 8),
		(uint8_t)(reg & 0xffU),
		(uint8_t)(value >> 8),
		(uint8_t)(value & 0xffU),
	};

	return i2c_write(i2c_dev, buf, sizeof(buf), NAU88L21_I2C_ADDR);
}

static int nau88l21_read_reg(const struct device *i2c_dev, uint16_t reg, uint16_t *value)
{
	uint8_t reg_buf[2] = {
		(uint8_t)(reg >> 8),
		(uint8_t)(reg & 0xffU),
	};
	uint8_t value_buf[2];
	int ret;

	if (value == NULL) {
		return -EINVAL;
	}

	ret = i2c_write_read(i2c_dev, NAU88L21_I2C_ADDR,
			     reg_buf, sizeof(reg_buf),
			     value_buf, sizeof(value_buf));
	if (ret < 0) {
		return ret;
	}

	*value = ((uint16_t)value_buf[0] << 8) | value_buf[1];

	return 0;
}

int nau88l21_init(const struct device *i2c_dev)
{
	int ret;
	volatile uint16_t general_status = 0U;

	if (i2c_dev == NULL) {
		return -EINVAL;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_RESET, 0x0001);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_RESET, 0x0001);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(1));

	ret = nau88l21_read_reg(i2c_dev, NAU88L21_REG_GENERAL_STATUS, (uint16_t *)&general_status);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_I2S_PCM_CTRL1, NAU88L21_I2S_PCM_CTRL1_I2S_16BIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_I2S_PCM_CTRL2, NAU88L21_I2S_PCM_CTRL2_I2S_48K);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_BOOST, NAU88L21_BOOST_ENABLE_ANALOG_BIAS);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_DAC_CONTROL, NAU88L21_DAC_CONTROL_ENABLE_BOTH);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CHARGE_PUMP_CONTROL, NAU88L21_CHARGE_PUMP_ENABLE_PRECHARGE);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL, NAU88L21_POWER_UP_OUTPUT_INTEGRATORS);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL, NAU88L21_POWER_UP_DRIVER_STAGE);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(5));

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL, NAU88L21_POWER_UP_DRIVER_MAIN);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(1));

	return 0;
}
