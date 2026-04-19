#include "audio_io/i2s.h"

#include "audio_io/nau88l21.h"

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define AUDIO_I2S_SAMPLE_RATE_HZ           48000U
#define AUDIO_I2S_CHANNELS                 2U
#define AUDIO_I2S_WORD_SIZE_BITS           16U
#define AUDIO_I2S_BYTES_PER_SAMPLE         2U
#define AUDIO_I2S_FRAME_SIZE               (AUDIO_I2S_CHANNELS * AUDIO_I2S_BYTES_PER_SAMPLE)
#define AUDIO_I2S_FRAMES_PER_BLOCK         192U
#define AUDIO_I2S_BLOCK_SIZE               (AUDIO_I2S_FRAMES_PER_BLOCK * AUDIO_I2S_FRAME_SIZE)
#define AUDIO_I2S_BLOCK_COUNT              4U
#define AUDIO_I2S_THREAD_STACK_SIZE        3072
#define AUDIO_I2S_THREAD_PRIORITY          8
#define AUDIO_I2S_TONE_FREQUENCY_HZ        1000U
#define AUDIO_I2S_TONE_AMPLITUDE           4000

K_MEM_SLAB_DEFINE_STATIC(audio_i2s_tx_slab, AUDIO_I2S_BLOCK_SIZE, AUDIO_I2S_BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(tdm));
static const struct device *const codec_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c23));

static volatile bool audio_i2s_ready;
static volatile bool audio_i2s_tone_enabled;
static bool audio_i2s_running;
static uint32_t audio_i2s_tone_frame_index;

static void audio_i2s_fill_tone_block(int16_t *samples, size_t frames)
{
	const uint32_t period_frames = AUDIO_I2S_SAMPLE_RATE_HZ / AUDIO_I2S_TONE_FREQUENCY_HZ;

	for (size_t frame = 0; frame < frames; ++frame) {
		int16_t sample = ((audio_i2s_tone_frame_index % period_frames) < (period_frames / 2U)) ?
			AUDIO_I2S_TONE_AMPLITUDE : -AUDIO_I2S_TONE_AMPLITUDE;

		samples[(frame * 2U) + 0U] = sample;
		samples[(frame * 2U) + 1U] = sample;
		audio_i2s_tone_frame_index++;
	}
}

static int audio_i2s_queue_tone_block(void)
{
	void *block = NULL;
	int ret;

	ret = k_mem_slab_alloc(&audio_i2s_tx_slab, &block, K_MSEC(20));
	if (ret < 0) {
		return ret;
	}

	audio_i2s_fill_tone_block((int16_t *)block, AUDIO_I2S_FRAMES_PER_BLOCK);

	ret = i2s_write(i2s_dev, block, AUDIO_I2S_BLOCK_SIZE);
	if (ret < 0) {
		k_mem_slab_free(&audio_i2s_tx_slab, block);
		return ret;
	}

	return 0;
}

static int audio_i2s_start(void)
{
	int ret;

	if (audio_i2s_running) {
		return 0;
	}

	audio_i2s_tone_frame_index = 0U;

	ret = audio_i2s_queue_tone_block();
	if (ret < 0) {
		return ret;
	}

	ret = audio_i2s_queue_tone_block();
	if (ret < 0) {
		return ret;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		return ret;
	}

	audio_i2s_running = true;
	return 0;
}

static void audio_i2s_stop(void)
{
	if (!audio_i2s_running) {
		return;
	}

	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	audio_i2s_running = false;
	audio_i2s_tone_frame_index = 0U;
}

static int audio_i2s_configure(void)
{
	struct i2s_config cfg = {
		.word_size = AUDIO_I2S_WORD_SIZE_BITS,
		.channels = AUDIO_I2S_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = AUDIO_I2S_SAMPLE_RATE_HZ,
		.mem_slab = &audio_i2s_tx_slab,
		.block_size = AUDIO_I2S_BLOCK_SIZE,
		.timeout = 20,
	};

	return i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
}

static void audio_i2s_thread(void *arg1, void *arg2, void *arg3)
{
	int ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (!device_is_ready(i2s_dev)) {
		printk("audio_i2s: TDM device not ready\n");
		return;
	}

	if (!device_is_ready(codec_i2c_dev)) {
		printk("audio_i2s: codec I2C device not ready\n");
		return;
	}

	ret = nau88l21_init(codec_i2c_dev);
	if (ret < 0) {
		printk("audio_i2s: codec init failed (%d: %s)\n", ret, strerror(-ret));
		return;
	}
	else {
		printk("audio_i2s: codec initialized (%d: %s)\n", ret, strerror(-ret));
	}

	ret = audio_i2s_configure();
	if (ret < 0) {
		printk("audio_i2s: I2S configure failed (%d: %s)\n", ret, strerror(-ret));
		return;
	}

	audio_i2s_ready = true;

	while (1) {
		if (!audio_i2s_tone_enabled) {
			audio_i2s_stop();
			k_sleep(K_MSEC(20));
			continue;
		}

		if (!audio_i2s_running) {
			ret = audio_i2s_start();
			if (ret < 0) {
				printk("audio_i2s: start failed (%d: %s)\n", ret, strerror(-ret));
				k_sleep(K_MSEC(100));
				continue;
			}
		}

		ret = audio_i2s_queue_tone_block();
		if (ret < 0) {
			printk("audio_i2s: write failed (%d: %s)\n", ret, strerror(-ret));
			audio_i2s_stop();
			k_sleep(K_MSEC(50));
		}
	}
}

bool audio_i2s_is_ready(void)
{
	return audio_i2s_ready;
}

bool audio_i2s_is_tone_enabled(void)
{
	return audio_i2s_tone_enabled;
}

void audio_i2s_set_tone_enabled(bool enabled)
{
	audio_i2s_tone_enabled = enabled;
}

K_THREAD_DEFINE(audio_i2s_thread_id, AUDIO_I2S_THREAD_STACK_SIZE, audio_i2s_thread,
		NULL, NULL, NULL, AUDIO_I2S_THREAD_PRIORITY, 0, 0);
