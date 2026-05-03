#include "audio/i2s.h"

#include "audio/nau88l21.h"
#include "common/tusb_fifo.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2s, LOG_LEVEL_INF);
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>

#define AUDIO_I2S_TX_DMA_BLOCK_COUNT       CONFIG_I2S_NRF_TDM_TX_BLOCK_COUNT
#define AUDIO_I2S_RX_DMA_BLOCK_COUNT       CONFIG_I2S_NRF_TDM_RX_BLOCK_COUNT
#define AUDIO_I2S_THREAD_STACK_SIZE        3072
#define AUDIO_I2S_THREAD_PRIORITY          4

#if NAU88L21_I2S_CODEC_CLOCK_MASTER
#define AUDIO_I2S_CLOCK_OPTIONS (I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE)
#else
#error "Only NAU88L21 codec-master I2S clocking is supported"
#endif

enum i2s_cmd_type { I2S_CMD_ACTIVATE, I2S_CMD_DEACTIVATE };

struct i2s_cmd {
	enum i2s_cmd_type type;
	tu_fifo_t *tx;
	tu_fifo_t *rx;
};

#define I2S_CMD_Q_SIZE 1
K_MSGQ_DEFINE(i2s_cmdq, sizeof(struct i2s_cmd), I2S_CMD_Q_SIZE, 4);

K_MEM_SLAB_DEFINE_STATIC(audio_i2s_tx_slab, AUDIO_I2S_BLOCK_BYTES,
			 AUDIO_I2S_TX_DMA_BLOCK_COUNT, 4);
K_MEM_SLAB_DEFINE_STATIC(audio_i2s_rx_slab, AUDIO_I2S_BLOCK_BYTES,
			 AUDIO_I2S_RX_DMA_BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(tdm));
static const struct device *const codec_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c23));

static bool audio_i2s_running;
static uint32_t audio_i2s_tx_pending_bytes;

BUILD_ASSERT((AUDIO_I2S_SAMPLE_RATE_HZ % 1000U) == 0U,
	     "AUDIO_I2S_SAMPLE_RATE_HZ must be divisible by 1000");
BUILD_ASSERT((AUDIO_I2S_BLOCK_BYTES % AUDIO_I2S_BYTES_PER_STEREO_SAMPLE) == 0U,
	     "AUDIO_I2S_BLOCK_BYTES must map to whole stereo samples");

static int audio_i2s_configure_direction(enum i2s_dir dir, struct k_mem_slab *slab, int32_t timeout_ms)
{
	struct i2s_config cfg = {
		.word_size = AUDIO_I2S_WORD_SIZE_BITS,
		.channels = AUDIO_I2S_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = AUDIO_I2S_CLOCK_OPTIONS,
		.frame_clk_freq = AUDIO_I2S_SAMPLE_RATE_HZ,
		.mem_slab = slab,
		.block_size = AUDIO_I2S_BLOCK_BYTES,
		.timeout = timeout_ms,
	};

	return i2s_configure(i2s_dev, dir, &cfg);
}

static int audio_i2s_configure(void)
{
	int ret;

	ret = audio_i2s_configure_direction(I2S_DIR_TX, &audio_i2s_tx_slab, 0);
	if (ret < 0) {
		return ret;
	}

	ret = audio_i2s_configure_direction(I2S_DIR_RX, &audio_i2s_rx_slab, SYS_FOREVER_MS);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int audio_i2s_tx_block(tu_fifo_t *tx_fifo)
{
	void *slab_block;
	int ret;

	ret = k_mem_slab_alloc(&audio_i2s_tx_slab, &slab_block, K_NO_WAIT);
	// ENOMEM, not a big deal. Will just get a free block next time. Early return.
	if (ret == -ENOMEM) {
		return ret;
	}
	else if (ret < 0) {
		LOG_ERR("Couldnt get mem slab! (%d: %s)", ret, strerror(-ret));
		return ret;
	}

	tu_fifo_read_n(tx_fifo, slab_block, AUDIO_I2S_BLOCK_BYTES);

	ret = i2s_write(i2s_dev, slab_block, AUDIO_I2S_BLOCK_BYTES);
	if (ret < 0) {
		LOG_ERR("i2s_write failed (%d: %s)", ret, strerror(-ret));
		k_mem_slab_free(&audio_i2s_tx_slab, slab_block);
	}
	else {
		audio_i2s_tx_pending_bytes += AUDIO_I2S_BLOCK_BYTES;
	}
	return ret;
}

static uint8_t extract_left_to_mono(const uint8_t *stereo, uint8_t stereo_samples, uint8_t *mono)
{
	if (stereo == NULL || mono == NULL) {
		return 0U;
	}

	for (uint8_t i = 0U; i < stereo_samples; ++i) {
		mono[i * 2U] = stereo[i * 4U];
		mono[i * 2U + 1U] = stereo[i * 4U + 1U];
	}

	return stereo_samples * 2U;
}

static int audio_i2s_rx_block(tu_fifo_t *rx_fifo)
{
	int ret;
	void *slab_block;
	size_t read_size;

	ret = i2s_read(i2s_dev, &slab_block, &read_size);
	if (ret < 0) {
		LOG_ERR("i2s_read failed (%d: %s)", ret, strerror(-ret));
		return ret;
	}

	__ASSERT_NO_MSG(read_size == AUDIO_I2S_BLOCK_BYTES);
	__ASSERT_NO_MSG(audio_i2s_tx_pending_bytes >= AUDIO_I2S_BLOCK_BYTES);
	audio_i2s_tx_pending_bytes -= AUDIO_I2S_BLOCK_BYTES;

	if (rx_fifo != NULL) {
		uint8_t mono_bytes = extract_left_to_mono(slab_block,
								AUDIO_I2S_STEREO_SAMPLES_PER_BLOCK,
								slab_block);
		if (mono_bytes > 0U) {
			tu_fifo_write_n(rx_fifo, slab_block, mono_bytes);
		}
	}

	k_mem_slab_free(&audio_i2s_rx_slab, slab_block);

	return ret;
}

static int audio_i2s_start(void)
{
	int ret;

	if (audio_i2s_running) {
		return 0;
	}

	for (int i = 0; i < 10; i++) {
		ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
		if (ret == 0) {
			audio_i2s_running = true;
			return 0;
		}
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_PREPARE);
		k_sleep(K_USEC(50));
	}

	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	return ret;
}

static void audio_i2s_stop(void)
{
	if (!audio_i2s_running) {
		return;
	}

	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	audio_i2s_running = false;
	audio_i2s_tx_pending_bytes = 0U;
}

static void audio_i2s_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	
	if (!device_is_ready(i2s_dev) || !device_is_ready(codec_i2c_dev)) {
		LOG_ERR("device not ready");
		return;
	}
	if (nau88l21_init(codec_i2c_dev) < 0) {
		LOG_ERR("codec init failed");
		return;
	}
	if (audio_i2s_configure() < 0) {
		LOG_ERR("configure failed");
		return;
	}
	LOG_INF("ready");

	while (1) {
		int ret;
		struct i2s_cmd cmd = {I2S_CMD_DEACTIVATE, NULL, NULL};

		/* Inactive loop: Wait for activation command, then enter active I/O loop until deactivation or error. */
        do {
			k_msgq_get(&i2s_cmdq, &cmd, K_FOREVER);
		} while (cmd.type != I2S_CMD_ACTIVATE);

		/* Prepare active loop */
		tu_fifo_t *tx_fifo = cmd.tx;
		tu_fifo_t *rx_fifo = cmd.rx;

		if(tx_fifo == NULL || rx_fifo == NULL) {
			LOG_WRN("invalid cmd with null fifo, going back to inactive state");
			continue;
		}

		/* Pre Fill */
		while(tu_fifo_count(tx_fifo) >= AUDIO_I2S_BLOCK_BYTES) {
			if (audio_i2s_tx_block(tx_fifo) < 0) break;
		}

		ret = audio_i2s_start();
		if (ret < 0) {
			LOG_ERR("start failed (%d: %s)", ret, strerror(-ret));
			audio_i2s_stop();
			continue;
		}

		/* Active loop: submit blocks from tx fifo, collect into rx fifo, watch for deactivate command. */
		while(1) {
			// Exit condition:
			if (k_msgq_get(&i2s_cmdq, &cmd, K_NO_WAIT) == 0 && cmd.type == I2S_CMD_DEACTIVATE) {
				audio_i2s_stop();
				break;
			}
			
			// Read
			if(audio_i2s_rx_block(rx_fifo) < 0) {
				audio_i2s_stop();
				break;
			}

			// Submit as many blocks as possible.
			while (tu_fifo_count(tx_fifo) >= AUDIO_I2S_BLOCK_BYTES) {
				if(audio_i2s_tx_block(tx_fifo) < 0) break;
			}
		}
	}
}

void audio_i2s_activate(tu_fifo_t *tx_fifo, tu_fifo_t *rx_fifo)
{
	struct i2s_cmd cmd = { .type = I2S_CMD_ACTIVATE, .tx = tx_fifo, .rx = rx_fifo };

	k_msgq_put(&i2s_cmdq, &cmd, K_FOREVER);
}

void audio_i2s_deactivate(void)
{
	struct i2s_cmd cmd = { .type = I2S_CMD_DEACTIVATE };

	k_msgq_put(&i2s_cmdq, &cmd, K_FOREVER);
}

uint32_t audio_i2s_tx_get_pending_bytes(void)
{
	return audio_i2s_tx_pending_bytes;
}

K_THREAD_DEFINE(audio_i2s_thread_id, AUDIO_I2S_THREAD_STACK_SIZE, audio_i2s_thread,
		NULL, NULL, NULL, AUDIO_I2S_THREAD_PRIORITY, 0, 0);
