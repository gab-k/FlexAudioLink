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

static K_SEM_DEFINE(audio_i2s_resume_sem, 0, 1);
/* k_uptime_get() of the last successful TX submit; 0 = never or stopped. */
static int64_t audio_i2s_last_tx_ms;

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

static void audio_i2s_tx_on_block_completed(void)
{
	if (audio_i2s_tx_pending_bytes >= AUDIO_I2S_BLOCK_BYTES) {
		audio_i2s_tx_pending_bytes -= AUDIO_I2S_BLOCK_BYTES;
	} else {
		audio_i2s_tx_pending_bytes = 0U;
	}
}

static int audio_i2s_submit_tx_block(void)
{
	tu_fifo_t *fifo;
	void *dma_block = NULL;
	int ret;

	fifo = audio_i2s_tx_fifo;

	if (fifo == NULL) {
		return -EAGAIN;
	}

	if (tu_fifo_count(fifo) < AUDIO_I2S_BLOCK_BYTES) {
		return -EAGAIN;
	}

	ret = k_mem_slab_alloc(&audio_i2s_slab, &dma_block, K_NO_WAIT);
	if (ret < 0) {
		return -EAGAIN;
	}
	tu_fifo_read_n(fifo, dma_block, AUDIO_I2S_BLOCK_BYTES);

	ret = i2s_write(i2s_dev, dma_block, AUDIO_I2S_BLOCK_BYTES);
	if (ret < 0) {
		k_mem_slab_free(&audio_i2s_slab, dma_block);
		return ret;
	}

	audio_i2s_tx_pending_bytes += AUDIO_I2S_BLOCK_BYTES;

	return 0;
}

static int audio_i2s_collect_rx_block(void)
{
	struct audio_i2s_block dst;
	void *dma_block = NULL;
	size_t size = 0U;
	int ret;

	ret = i2s_read(i2s_dev, &dma_block, &size);
	if (ret == -EIO) {
		return -EAGAIN;
	}
	if (ret < 0) {
		return ret;
	}

	memset(&dst, 0, sizeof(dst));
	memcpy(&dst, dma_block, MIN(size, sizeof(dst)));

	audio_i2s_tx_on_block_completed();

	/* Write the stereo block into the path's mic FIFO (if set). */
	if (audio_i2s_rx_fifo != NULL) {
		tu_fifo_write_n(audio_i2s_rx_fifo, dst.bytes, AUDIO_I2S_BLOCK_BYTES);
	}

	k_mem_slab_free(&audio_i2s_slab, dma_block);
	return 0;
}

static int audio_i2s_start(void)
{
	int ret;

	if (audio_i2s_running) {
		return 0;
	}

	ret = audio_i2s_submit_tx_block();
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

	printk("audio_i2s: codec initialized\n");

	ret = audio_i2s_configure();
	if (ret < 0) {
		printk("audio_i2s: I2S configure failed (%d: %s)\n", ret, strerror(-ret));
		return;
	}

	while (1) {
		if (!audio_i2s_running) {
			ret = audio_i2s_start();
			if (ret == -EAGAIN) {
				/* No data available yet — park on the resume
				 * semaphore.  The wired path gives it when it
				 * transitions BUFFERING -> PLAYING. */
				(void)k_sem_take(&audio_i2s_resume_sem, K_NO_WAIT);
				k_sem_take(&audio_i2s_resume_sem, K_FOREVER);
				continue;
			}
			if (ret < 0) {
				printk("audio_i2s: start failed (%d: %s)\n", ret, strerror(-ret));
				k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
				continue;
			}
			audio_i2s_last_tx_ms = k_uptime_get();
		}

		int tx_ret = audio_i2s_submit_tx_block();
		int rx_ret = audio_i2s_collect_rx_block();

		if (tx_ret < 0 && tx_ret != -EAGAIN && tx_ret != -EBUSY) {
			printk("audio_i2s: write failed (%d: %s)\n",
			       tx_ret, strerror(-tx_ret));
			audio_i2s_stop();
			audio_i2s_configure();
			k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
			continue;
		}
		if (rx_ret < 0 && rx_ret != -EAGAIN && rx_ret != -EBUSY) {
			printk("audio_i2s: read failed (%d: %s)\n",
			       rx_ret, strerror(-rx_ret));
			audio_i2s_stop();
			audio_i2s_configure();
			k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
			continue;
		}

		if (tx_ret == 0) {
			audio_i2s_last_tx_ms = k_uptime_get();
		}

		/* Starvation: TX FIFO is dry, pipeline empty,
		 * and no successful TX for > 2 ms.  Notify the upper layer,
		 * stop the peripheral, and park until audio_i2s_resume(). */
		if (tx_ret == -EAGAIN && audio_i2s_running) {
			bool starved = false;
			tu_fifo_t *ff = audio_i2s_tx_fifo;
			if (ff != NULL &&
			    tu_fifo_count(ff) < AUDIO_I2S_BLOCK_BYTES &&
			    audio_i2s_tx_pending_bytes == 0U &&
			    audio_i2s_last_tx_ms != 0 &&
			    k_uptime_get() - audio_i2s_last_tx_ms > 2) {
				starved = true;
			}

			if (starved) {
				audio_i2s_stop();
				audio_i2s_configure();
				audio_i2s_tx_flush();
				audio_i2s_last_tx_ms = 0;
				(void)k_sem_take(&audio_i2s_resume_sem, K_NO_WAIT);
				k_sem_take(&audio_i2s_resume_sem, K_FOREVER);
				continue;
			}
		}

		if (tx_ret != 0 && rx_ret < 0) {
			k_sleep(K_USEC(AUDIO_I2S_RETRY_SLEEP_US));
		}
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

void audio_i2s_tx_flush(void)
{
	audio_i2s_tx_pending_bytes = 0U;
}

void audio_i2s_rx_set_fifo(tu_fifo_t *dest)
{
	audio_i2s_rx_fifo = dest;
}

void audio_i2s_resume(void)
{
	k_sem_give(&audio_i2s_resume_sem);
}

K_THREAD_DEFINE(audio_i2s_thread_id, AUDIO_I2S_THREAD_STACK_SIZE, audio_i2s_thread,
		NULL, NULL, NULL, AUDIO_I2S_THREAD_PRIORITY, 0, 0);
