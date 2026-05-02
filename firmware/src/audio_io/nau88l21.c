#include "audio_io/nau88l21.h"

#include "audio_io/i2s.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define NAU88L21_I2C_ADDR 0x1B

/* The datasheet prose and register table disagree on reset wording.
 * The register table places the reset register at 0x00, so use that as the
 * authoritative address for the initial defensive reset in bring-up.
 */
#define NAU88L21_REG_RESET                         0x00
#define NAU88L21_REG_ENA_CTRL                      0x01
#define NAU88L21_REG_CLK_DIVIDER                   0x03
#define NAU88L21_REG_FLL1                          0x04
#define NAU88L21_REG_FLL3                          0x06
#define NAU88L21_REG_FLL4                          0x07
#define NAU88L21_REG_FLL5                          0x08
#define NAU88L21_REG_FLL6                          0x09
#define NAU88L21_REG_FLL7                          0x0A
#define NAU88L21_REG_FLL8                          0x0B
#define NAU88L21_REG_JACK_DET_CTRL                 0x0D
#define NAU88L21_REG_I2S_PCM_CTRL1                 0x1C
#define NAU88L21_REG_I2S_PCM_CTRL2                 0x1D
#define NAU88L21_REG_LEFT_TIME_SLOT                0x1E
#define NAU88L21_REG_CLASSG_CTRL                   0x4B
#define NAU88L21_REG_BIAS_ADJ                      0x66
#define NAU88L21_REG_ANALOG_CONTROL_2              0x6A
#define NAU88L21_REG_DAC_CONTROL                   0x73
#define NAU88L21_REG_BOOST                         0x76
#define NAU88L21_REG_POWER_UP_CONTROL              0x7F
#define NAU88L21_REG_CHARGE_PUMP_CONTROL           0x80
#define NAU88L21_REG_GENERAL_STATUS                0x82

#define NAU88L21_GENERAL_STATUS_FLL_LOCK_MASK      BIT(5)
#define NAU88L21_FLL_LOCK_POLL_TIMEOUT_MS          100U
#define NAU88L21_FLL_LOCK_POLL_INTERVAL_MS         2U

BUILD_ASSERT(NAU88L21_I2S_CODEC_CLOCK_MASTER,
	     "NAU88L21 register table expects codec-master I2S clocking");

/*
 * --- Adaptive codec clock (FLL) tuning constants ---------------------------------
 *
 * The FLL is driven from the 32 MHz MCLKI reference.
 *   FREF_eff  = 32 MHz / 4 (FLL4 pre-scale)        = 8 MHz
 *   FLL_RATIO = 1
 *   FLL_INT   = 12
 *   FLL_FRAC  = 24-bit (FLL7[7:0] | FLL8[15:0])
 *
 * FDCO  = FREF_eff * (INT + FRAC / 2^24)
 * MCLK  = FDCO / 8        (SYSCLK=/2, MCLK_SRC=/4)
 * Fs    = MCLK / 256 = FDCO / 2048
 *
 * At nominal 48 kHz:
 *   FDCO_nom  = 48e3 * 2048 = 98,304,000 Hz
 *   MCLK_nom  = 12,288,000 Hz
 *   FRAC_nom  = 0x49BA5E  (4,831,838)
 *
 * Each FRAC LSB changes FDCO by ~0.477 Hz -> Fs by ~0.233 mHz.
 * A ±100 Hz adjustment needs a ±~429,200 FRAC delta.
 * FDCO must stay within 90-100 MHz; ±100 Hz at 48 kHz keeps it well within bounds.
 */
#define NAU88L21_FREF_EFF_HZ                       8000000U
#define NAU88L21_FLL_INT                           12U
#define NAU88L21_FLL_FRAC_BITS                     24U
#define NAU88L21_FLL_FRAC_SCALE                    (1ULL << NAU88L21_FLL_FRAC_BITS)
#define NAU88L21_FDCO_DIV_FROM_FS                  2048U
#define NAU88L21_FLL_FRAC_NOMINAL                  0x0049BA5EULL

#define NAU88L21_FLL_MIN_FDCO_HZ                   90000000U
#define NAU88L21_FLL_MAX_FDCO_HZ                   100000000U

/*
 * Widenable on-the-fly adjustment (Hz at Fs).
 * INT=12 with 0 ≤ FRAC ≤ 8,388,608 keeps FDCO in 90-100 MHz,
 * giving Fs from 46875 Hz to 48828 Hz, i.e. -1125 Hz to +828 Hz.
 */
#define NAU88L21_FLL_MAX_RATE_ADJUST_HZ            800

static const struct device *nau88l21_i2c_dev;

static uint32_t nau88l21_compute_fll_frac(int32_t target_rate_hz)
{
	uint64_t fdco_hz;
	uint64_t frac;

	fdco_hz = (uint64_t)target_rate_hz * NAU88L21_FDCO_DIV_FROM_FS;

	frac = fdco_hz * NAU88L21_FLL_FRAC_SCALE / NAU88L21_FREF_EFF_HZ;
	frac -= (uint64_t)NAU88L21_FLL_INT * NAU88L21_FLL_FRAC_SCALE;

	return (uint32_t)frac;
}

/* Playback bring-up for NAU88L21:
 * - standard I2S
 * - 16-bit stereo
 * - codec master mode (48 kHz) using FLL from 32 MHz MCLKI
 * - DAC/headphone path powered up
 *
 * These values are derived from the datasheet register tables and kept
 * intentionally minimal.
 */
#define NAU88L21_I2S_PCM_CTRL1_I2S_16BIT           0x0002
/* FLL for 32 MHz reference:
 * - keep FLL tuning defaults, but force ratio=1 and retune integer/fraction.
 * - FLL4 pre-scale divider is REG0x07[11:10]; set to 10b (1/4), so FREF=8 MHz.
 * - FDCO target is 98.304 MHz, then MCLK_SRC=1/4 => 12.288 MHz.
 */
#define NAU88L21_FLL1_RATIO_1_ICTRL_6              0x1801
#define NAU88L21_FLL3_INTEGER_12                   0x000C
#define NAU88L21_FLL4_REFDIV_1_4                   0x8860
#define NAU88L21_FLL5_LOOP_FILTER_FRACTIONAL       0xC000
#define NAU88L21_FLL6_FRACTIONAL_SDM_CUTOFF500     0x6000
#define NAU88L21_CLK_DIVIDER_MCLK_48K_SETUP        0x0053
/* Nominal 48 kHz FLL fractional (assuming 32 MHz MCLKI).
 * FRAC = 0x49BA5E → FDCO = 98,304,000 Hz → Fs = 48,000 Hz.
 * Per-board crystal tolerance means actual Fs may be ±300 Hz off;
 * the P-controller compensates automatically. */
#define NAU88L21_FLL7_FRAC_H_32M                   0x0049
#define NAU88L21_FLL8_FRAC_L_32M                   0xBA5E
/* REG0x03: SYSCLK from 1/2 DCO, MCLK_SRC = 1/4 (12.288 MHz from FLL DCO). */
#define NAU88L21_CLK_DIVIDER_FLL_48K               0x8053
/* REG0x1D:
 * - master mode
 * - BCLK=MCLK/8 (1.536 MHz), LRCK=BCLK/32 for 48 kHz -> 16 BCLK per channel
 *   slot. Matches nRF TDM slave I2S geometry (sample_width=16 => 16 BCLK/slot).
 * - clear I2S_TRI from default high-Z state
 * - force clock outputs enabled (I2S_DRV=1) during bring-up
 * Field layout: bit14 DRV=1, bits[13:12] LRC_DIV=11 (/32), bit4=1 (reserved
 * NAU88L21 bit kept as seen in prior working-clocks readback), bit3 MS=1,
 * bits[2:0] BLK_DIV=011 (MCLK/8).
 */
#define NAU88L21_I2S_PCM_CTRL2_MASTER_48K_16BIT    0x701B
/* Output path bring-up values. Static init programs bias/clocks/pump, then a
 * staged power-up sequence enables the DAC and HP driver chain:
 *   stage 0: R4B Class G DAC enables
 *   stage 1: R80 charge-pump enable, 20 ms settle, JAMNODCLOW
 *   stage 2: R73 analog DACL/R
 *   stage 3: R73 analog DAC clocks
 *   stage 4-6: R7F integrators -> input stages -> main drivers
 *   stage 7: R80 "Output DACL/R" (POWER_DOWN_DACL/R bits, misleadingly named)
 *   stage 9: R76 HP boost driver, R4B Class G enable,
 *            R66 clear BIAS_TESTDAC_EN so DAC signal reaches the HP path
 *   final:   R01 EN_DACL/R
 */
#define NAU88L21_BIAS_ADJ_INIT                     0x0350
/* Clear BIAS_TESTDAC_EN (R66[9:8]) so the DAC signal can reach the HP path. */
#define NAU88L21_BIAS_ADJ_PLAYBACK                 0x0050
#define NAU88L21_LEFT_TIME_SLOT_INIT               0x2000
#define NAU88L21_BOOST_INIT                        0x3340
#define NAU88L21_BOOST_PLAYBACK                    0x3140
#define NAU88L21_CLASSG_CTRL_TIMER                 0x2000
#define NAU88L21_CLASSG_CTRL_HP_AMPS               0x2006
#define NAU88L21_CLASSG_CTRL_PLAYBACK              0x2007
#define NAU88L21_ANALOG_CONTROL_2_INIT             0x1003
#define NAU88L21_RDAC_INIT                         0x0034
#define NAU88L21_RDAC_DAC_EN                       0x3034
#define NAU88L21_RDAC_PLAYBACK                     0x3334
#define NAU88L21_CHARGE_PUMP_CLEAR_PD              0x0000
#define NAU88L21_CHARGE_PUMP_EN                    0x0020
#define NAU88L21_CHARGE_PUMP_PLAYBACK              0x0420
/* Setting R80 POWER_DOWN_DACL/R (bits 8,9) to 1 actually enables the DAC
 * output stage despite the misleading register-field name.
 */
#define NAU88L21_CHARGE_PUMP_OUTPUT_DAC            0x0720
/* SPKR_DWN1L/R=1 un-grounds HPL/HPR. Leaving them 0 shorts HP to GND -> silence. */
#define NAU88L21_JACK_DET_CTRL_UNGROUND_HP         0xC000
#define NAU88L21_POWER_UP_INTEGRATORS              0x0030
#define NAU88L21_POWER_UP_DRIVER_INSTG             0x003C
#define NAU88L21_POWER_UP_DRIVER_MAIN              0x003F
#define NAU88L21_ENA_CTRL_EN_DAC_MASK              0x0C00

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
	uint16_t fll_status = 0U;
	uint32_t fll_wait_ms = 0U;
	bool fll_locked = false;

	if (i2c_dev == NULL) {
		return -EINVAL;
	}

	nau88l21_i2c_dev = i2c_dev;

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_RESET, 0x0001);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(1));

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_I2S_PCM_CTRL1, NAU88L21_I2S_PCM_CTRL1_I2S_16BIT);
	if (ret < 0) {
		return ret;
	}

	/* Keep SYSCLK on MCLK pin while programming FLL. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CLK_DIVIDER,
				 NAU88L21_CLK_DIVIDER_MCLK_48K_SETUP);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL1, NAU88L21_FLL1_RATIO_1_ICTRL_6);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL3, NAU88L21_FLL3_INTEGER_12);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL4, NAU88L21_FLL4_REFDIV_1_4);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Write fractional FIRST, then FLL5/FLL6 LAST.
	 * Nuvoton FLLs latch the divider registers on the trailing
	 * control-register writes — matching the NAU8821/NAU88L21
	 * reference driver sequence.  Writing FLL5/FLL6 *before*
	 * FLL7/FLL8 (as the prior code did) leaves the fractional
	 * divider at its POR default (~47850 Hz) regardless of what
	 * I2C readback shows.
	 */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL7, NAU88L21_FLL7_FRAC_H_32M);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL8, NAU88L21_FLL8_FRAC_L_32M);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL5, NAU88L21_FLL5_LOOP_FILTER_FRACTIONAL);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_FLL6, NAU88L21_FLL6_FRACTIONAL_SDM_CUTOFF500);
	if (ret < 0) {
		return ret;
	}

	/* Let the FLL settle before switching SYSCLK source to the FLL output. */
	k_sleep(K_MSEC(2));

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CLK_DIVIDER, NAU88L21_CLK_DIVIDER_FLL_48K);
	if (ret < 0) {
		return ret;
	}

	/* Poll FLL lock after clocking switches to the FLL path. */
	for (fll_wait_ms = 0U; fll_wait_ms <= NAU88L21_FLL_LOCK_POLL_TIMEOUT_MS;
	     fll_wait_ms += NAU88L21_FLL_LOCK_POLL_INTERVAL_MS) {
		ret = nau88l21_read_reg(i2c_dev, NAU88L21_REG_GENERAL_STATUS, &fll_status);
		if (ret < 0) {
			return ret;
		}

		if ((fll_status & NAU88L21_GENERAL_STATUS_FLL_LOCK_MASK) != 0U) {
			fll_locked = true;
			break;
		}

		if (fll_wait_ms == NAU88L21_FLL_LOCK_POLL_TIMEOUT_MS) {
			break;
		}

		k_sleep(K_MSEC(NAU88L21_FLL_LOCK_POLL_INTERVAL_MS));
	}

	if (!fll_locked) {
		printk("nau88l21: WARN FLL not locked within %u ms (status=0x%04x)\n",
		       NAU88L21_FLL_LOCK_POLL_TIMEOUT_MS, fll_status);
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_I2S_PCM_CTRL2,
				 NAU88L21_I2S_PCM_CTRL2_MASTER_48K_16BIT);
	if (ret < 0) {
		return ret;
	}

	/* Static output-path init. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_BIAS_ADJ, NAU88L21_BIAS_ADJ_INIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_LEFT_TIME_SLOT,
				 NAU88L21_LEFT_TIME_SLOT_INIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_BOOST, NAU88L21_BOOST_INIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CLASSG_CTRL, NAU88L21_CLASSG_CTRL_TIMER);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_ANALOG_CONTROL_2,
				 NAU88L21_ANALOG_CONTROL_2_INIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_DAC_CONTROL, NAU88L21_RDAC_INIT);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CHARGE_PUMP_CONTROL,
				 NAU88L21_CHARGE_PUMP_CLEAR_PD);
	if (ret < 0) {
		return ret;
	}

	/* Un-ground HPL/HPR (clear SPKR_DWN1L/R in JACK_DET_CTRL). */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_JACK_DET_CTRL,
				 NAU88L21_JACK_DET_CTRL_UNGROUND_HP);
	if (ret < 0) {
		return ret;
	}

	/* Stage 0: HP amp L/R (Class G DAC enables). */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CLASSG_CTRL, NAU88L21_CLASSG_CTRL_HP_AMPS);
	if (ret < 0) {
		return ret;
	}

	/* Stage 1: Charge pump enable, then JAMNODCLOW after 20 ms (pump PMU event). */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CHARGE_PUMP_CONTROL,
				 NAU88L21_CHARGE_PUMP_EN);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(20));

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CHARGE_PUMP_CONTROL,
				 NAU88L21_CHARGE_PUMP_PLAYBACK);
	if (ret < 0) {
		return ret;
	}

	/* Stage 2: analog DACL/R. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_DAC_CONTROL, NAU88L21_RDAC_DAC_EN);
	if (ret < 0) {
		return ret;
	}

	/* Stage 3: analog DAC clocks. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_DAC_CONTROL, NAU88L21_RDAC_PLAYBACK);
	if (ret < 0) {
		return ret;
	}

	/* Stages 4..6: output driver integrators, input stages, main drivers. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL,
				 NAU88L21_POWER_UP_INTEGRATORS);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL,
				 NAU88L21_POWER_UP_DRIVER_INSTG);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(5));

	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_POWER_UP_CONTROL,
				 NAU88L21_POWER_UP_DRIVER_MAIN);
	if (ret < 0) {
		return ret;
	}

	/* Stage 9: enable HP boost driver (clear HP_BOOST_DIS). */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_BOOST, NAU88L21_BOOST_PLAYBACK);
	if (ret < 0) {
		return ret;
	}

	/* Ungate Class G. */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CLASSG_CTRL,
				 NAU88L21_CLASSG_CTRL_PLAYBACK);
	if (ret < 0) {
		return ret;
	}

	/* Let the DAC signal pass through (clear BIAS_TESTDAC_EN). */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_BIAS_ADJ,
				 NAU88L21_BIAS_ADJ_PLAYBACK);
	if (ret < 0) {
		return ret;
	}

	/* Stage 7: assert "Output DACL/R" (R80 POWER_DOWN_DACL/R, misleadingly named
	 * — setting them is what enables DAC output to drive the HP path).
	 */
	ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_CHARGE_PUMP_CONTROL,
				 NAU88L21_CHARGE_PUMP_OUTPUT_DAC);
	if (ret < 0) {
		return ret;
	}

	/* Enable digital DACL/R in ENA_CTRL (preserve other default enables). */
	{
		uint16_t ena_ctrl = 0U;

		ret = nau88l21_read_reg(i2c_dev, NAU88L21_REG_ENA_CTRL, &ena_ctrl);
		if (ret < 0) {
			return ret;
		}
		ret = nau88l21_write_reg(i2c_dev, NAU88L21_REG_ENA_CTRL,
					 ena_ctrl | NAU88L21_ENA_CTRL_EN_DAC_MASK);
		if (ret < 0) {
			return ret;
		}
	}

	k_sleep(K_MSEC(20));

	return 0;
}

int nau88l21_set_fll_target_rate_hz(int32_t target_rate_hz)
{
	uint32_t frac;
	uint16_t frac_high;
	uint16_t frac_low;
	uint64_t fdco_check;
	int ret;

	if (nau88l21_i2c_dev == NULL) {
		return -ENODEV;
	}

	if (target_rate_hz < (int32_t)(AUDIO_I2S_SAMPLE_RATE_HZ - NAU88L21_FLL_MAX_RATE_ADJUST_HZ) ||
	    target_rate_hz > (int32_t)(AUDIO_I2S_SAMPLE_RATE_HZ + NAU88L21_FLL_MAX_RATE_ADJUST_HZ)) {
		return -EINVAL;
	}

	frac = nau88l21_compute_fll_frac(target_rate_hz);

	fdco_check = (uint64_t)target_rate_hz * NAU88L21_FDCO_DIV_FROM_FS;
	if (fdco_check < NAU88L21_FLL_MIN_FDCO_HZ || fdco_check > NAU88L21_FLL_MAX_FDCO_HZ) {
		printk("nau88l21: FLL target rate %d Hz -> FDCO %llu Hz out of range\n",
		       target_rate_hz, (unsigned long long)fdco_check);
		return -ERANGE;
	}

	frac_high = (uint16_t)((frac >> 16) & 0xFFU);
	frac_low  = (uint16_t)(frac & 0xFFFFU);

	/*
	 * On-the-fly fractional update: write the new FLL7/FLL8 and
	 * let the FLL servo to the new target through its loop filter.
	 * No clock-domain switch, no control-register toggling.
	 */
	ret = nau88l21_write_reg(nau88l21_i2c_dev, NAU88L21_REG_FLL7, frac_high);
	if (ret < 0) {
		return ret;
	}

	ret = nau88l21_write_reg(nau88l21_i2c_dev, NAU88L21_REG_FLL8, frac_low);
	if (ret < 0) {
		return ret;
	}

	return 0;
}
