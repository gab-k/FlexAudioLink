#include "audio_io/i2s.h"

#include "audio_io/nau88l21.h"
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>

#define AUDIO_I2S_DMA_BLOCK_COUNT          32U
#define AUDIO_I2S_THREAD_STACK_SIZE        3072
#define AUDIO_I2S_THREAD_PRIORITY          8
#define AUDIO_I2S_RESTART_BACKOFF_MS       50
#define AUDIO_I2S_RETRY_SLEEP_US           100

#if NAU88L21_I2S_CODEC_CLOCK_MASTER
#define AUDIO_I2S_CLOCK_OPTIONS (I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE)
#else
#error "Only NAU88L21 codec-master I2S clocking is supported"
#endif

K_MEM_SLAB_DEFINE_STATIC(audio_i2s_slab, AUDIO_I2S_BLOCK_BYTES, AUDIO_I2S_DMA_BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(tdm));
static const struct device *const codec_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c23));

static bool audio_i2s_running;
static tu_fifo_t *audio_i2s_tx_fifo;
/* Bytes copied out of upstream FIFOs but not yet clocked out of I2S TX.
 * Lives strictly in the I2S domain — upstream read pointers have already
 * advanced at enqueue time. */
static uint32_t audio_i2s_tx_pending_bytes;
/* Mic FIFO set by the active audio path — I2S writes completed RX blocks
 * here.  NULL means drop mic data. */
static tu_fifo_t *audio_i2s_rx_fifo;

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

	ret = audio_i2s_configure_direction(I2S_DIR_TX, &audio_i2s_slab, 0);
	if (ret < 0) {
		return ret;
	}

	ret = audio_i2s_configure_direction(I2S_DIR_RX, &audio_i2s_slab, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int audio_i2s_submit_tx_block(void)
{
	int ret;
	void *slab_block;
	tu_fifo_t *src_ff = audio_i2s_tx_fifo;


	if (src_ff == NULL) {
		return -EAGAIN;
	}

	if (tu_fifo_count(src_ff) < AUDIO_I2S_BLOCK_BYTES) {
		return -EAGAIN;
	}

	ret = k_mem_slab_alloc(&audio_i2s_slab, &slab_block, K_NO_WAIT);
	if (ret < 0) {
		return -EAGAIN;
	}
	tu_fifo_read_n(src_ff, slab_block, AUDIO_I2S_BLOCK_BYTES);

	ret = i2s_write(i2s_dev, slab_block, AUDIO_I2S_BLOCK_BYTES);
	if (ret < 0) {
		k_mem_slab_free(&audio_i2s_slab, slab_block);
		return ret;
	}

	audio_i2s_tx_pending_bytes += AUDIO_I2S_BLOCK_BYTES;

	return 0;
}

static int audio_i2s_collect_rx_block(void)
{
	int ret;
	size_t read_size;
	void *slab_block;
	tu_fifo_t *dst_ff = audio_i2s_rx_fifo;

	ret = i2s_read(i2s_dev, &slab_block, &read_size);
	if (ret == -EIO) {
		return -EAGAIN;
	}
	if (ret < 0) {
		return ret;
	}

	__ASSERT_NO_MSG(read_size == AUDIO_I2S_BLOCK_BYTES);

	__ASSERT_NO_MSG(audio_i2s_tx_pending_bytes >= AUDIO_I2S_BLOCK_BYTES);
	audio_i2s_tx_pending_bytes -= AUDIO_I2S_BLOCK_BYTES;

	if (dst_ff != NULL) {
		uint16_t w = tu_fifo_write_n(dst_ff, slab_block, AUDIO_I2S_BLOCK_BYTES);
		if (w < AUDIO_I2S_BLOCK_BYTES) {
			printk("audio_i2s: rx mic fifo overflow (%u/%u)\n",
			       w, (unsigned)AUDIO_I2S_BLOCK_BYTES);
		}
	}

	k_mem_slab_free(&audio_i2s_slab, slab_block);
	return 0;
}

static int audio_i2s_start(void)
{
	int ret;

	if (audio_i2s_running) {
		return 0;
	}

	ret = audio_i2s_submit_tx_block();
	if (ret == -ENOMSG) {
		/* Driver tx_queue still full from a prior cycle (peripheral
		 * stop/clock release is async). Purge and let caller retry. */
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		return -EAGAIN;
	}
	if (ret < 0) {
		return ret;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret < 0) {
		(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
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

	(void)i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	audio_i2s_running = false;
	audio_i2s_tx_pending_bytes = 0U;
}

static void audio_i2s_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	if (!device_is_ready(i2s_dev) || !device_is_ready(codec_i2c_dev)) {
		printk("audio_i2s: device not ready\n");
		return;
	}
	if (nau88l21_init(codec_i2c_dev) < 0) {
		printk("audio_i2s: codec init failed\n");
		return;
	}
	if (audio_i2s_configure() < 0) {
		printk("audio_i2s: configure failed\n");
		return;
	}
	printk("audio_i2s: ready\n");

	while (1) {
		int ret = audio_i2s_start();
		if (ret < 0) {
			printk("audio_i2s: start failed (%d: %s)\n", ret, strerror(-ret));
			k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
			continue;
		}

		while (1) {
			if(audio_i2s_tx_fifo == NULL) {
				break;
			}

			int tx_ret = audio_i2s_submit_tx_block();
			int rx_ret = audio_i2s_collect_rx_block();

			if (tx_ret < 0 && tx_ret != -EAGAIN) {
				printk("audio_i2s: write failed (%d: %s)\n", tx_ret, strerror(-tx_ret));
				break;
			}
			if (rx_ret < 0 && rx_ret != -EAGAIN) {
				printk("audio_i2s: read failed (%d: %s)\n", rx_ret, strerror(-rx_ret));
				break;
			}

			k_sleep(K_USEC(AUDIO_I2S_RETRY_SLEEP_US));
		}

		audio_i2s_stop();
		audio_i2s_configure();
	}
}

void audio_i2s_tx_set_fifo(tu_fifo_t *src)
{
	audio_i2s_tx_fifo = src;
}

uint32_t audio_i2s_tx_get_pending_bytes(void)
{
	return audio_i2s_tx_pending_bytes;
}

void audio_i2s_rx_set_fifo(tu_fifo_t *dest)
{
	audio_i2s_rx_fifo = dest;
}

K_THREAD_DEFINE(audio_i2s_thread_id, AUDIO_I2S_THREAD_STACK_SIZE, audio_i2s_thread,
		NULL, NULL, NULL, AUDIO_I2S_THREAD_PRIORITY, 0, 0);
