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

#define AUDIO_I2S_DMA_BLOCK_COUNT          4U
#define AUDIO_I2S_TX_QUEUE_DEPTH           12U
#define AUDIO_I2S_RX_QUEUE_DEPTH           12U
#define AUDIO_I2S_THREAD_STACK_SIZE        3072
#define AUDIO_I2S_THREAD_PRIORITY          8
#define AUDIO_I2S_RESTART_BACKOFF_MS       50
#define AUDIO_I2S_RX_TIMEOUT_MS            1

#if NAU88L21_I2S_CODEC_CLOCK_MASTER
#define AUDIO_I2S_CLOCK_OPTIONS (I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE)
#else
#error "Only NAU88L21 codec-master I2S clocking is supported"
#endif

K_MEM_SLAB_DEFINE_STATIC(audio_i2s_tx_slab, AUDIO_I2S_BLOCK_BYTES, AUDIO_I2S_DMA_BLOCK_COUNT, 4);
K_MEM_SLAB_DEFINE_STATIC(audio_i2s_rx_slab, AUDIO_I2S_BLOCK_BYTES, AUDIO_I2S_DMA_BLOCK_COUNT, 4);

K_MSGQ_DEFINE(audio_i2s_tx_msgq, sizeof(struct audio_i2s_block), AUDIO_I2S_TX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(audio_i2s_rx_msgq, sizeof(struct audio_i2s_block), AUDIO_I2S_RX_QUEUE_DEPTH, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(tdm));
static const struct device *const codec_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c23));

static volatile bool audio_i2s_ready;
static bool audio_i2s_running;

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

	ret = audio_i2s_configure_direction(I2S_DIR_TX, &audio_i2s_tx_slab, 20);
	if (ret < 0) {
		return ret;
	}

	/* tdm_nrf maps K_NO_WAIT RX underflow to -EIO. Use a short timeout so
	 * no-data is surfaced as -EAGAIN and handled as transient. */
	return audio_i2s_configure_direction(I2S_DIR_RX, &audio_i2s_rx_slab, AUDIO_I2S_RX_TIMEOUT_MS);
}

static int audio_i2s_submit_tx_block(void)
{
	struct audio_i2s_block src;
	void *dma_block = NULL;
	int ret;

	ret = k_mem_slab_alloc(&audio_i2s_tx_slab, &dma_block, K_MSEC(20));
	if (ret < 0) {
		return ret;
	}

	ret = k_msgq_get(&audio_i2s_tx_msgq, &src, K_NO_WAIT);
	if (ret < 0) {
		memset(&src, 0, sizeof(src));
	}

	memcpy(dma_block, &src, sizeof(src));

	ret = i2s_write(i2s_dev, dma_block, AUDIO_I2S_BLOCK_BYTES);
	if (ret < 0) {
		k_mem_slab_free(&audio_i2s_tx_slab, dma_block);
		return ret;
	}

	return 0;
}

static int audio_i2s_publish_rx_block(const struct audio_i2s_block *block)
{
	struct audio_i2s_block dropped;

	if (k_msgq_put(&audio_i2s_rx_msgq, block, K_NO_WAIT) == 0) {
		return 0;
	}

	/* Drop oldest block so readers receive the newest captured data. */
	(void)k_msgq_get(&audio_i2s_rx_msgq, &dropped, K_NO_WAIT);
	(void)k_msgq_put(&audio_i2s_rx_msgq, block, K_NO_WAIT);
	return 1;
}

static int audio_i2s_collect_rx_block(void)
{
	struct audio_i2s_block dst;
	void *dma_block = NULL;
	size_t size = 0U;
	size_t copy_size;
	int ret;

	ret = i2s_read(i2s_dev, &dma_block, &size);
	if (ret == -EIO) {
		/* RX underflow/no-data can surface as -EIO on some tdm_nrf paths. */
		return -EAGAIN;
	}
	if (ret < 0) {
		return ret;
	}

	memset(&dst, 0, sizeof(dst));
	copy_size = MIN(size, sizeof(dst));
	memcpy(&dst, dma_block, copy_size);
	ret = audio_i2s_publish_rx_block(&dst);

	k_mem_slab_free(&audio_i2s_rx_slab, dma_block);
	return ret;
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

	audio_i2s_ready = true;

	while (1) {
		if (!audio_i2s_running) {
			ret = audio_i2s_start();
			if (ret < 0) {
				printk("audio_i2s: start failed (%d: %s)\n", ret, strerror(-ret));
				k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
				continue;
			}
		}

		ret = audio_i2s_submit_tx_block();
		if (ret == -EAGAIN || ret == -EBUSY) {
			/* TX queue backpressure: keep stream running and retry. */
			k_sleep(K_MSEC(1));
			continue;
		}
		if (ret < 0) {
			printk("audio_i2s: write failed (%d: %s)\n", ret, strerror(-ret));
			audio_i2s_stop();
			k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
			continue;
		}

		ret = audio_i2s_collect_rx_block();
		if (ret < 0 && ret != -EAGAIN && ret != -EBUSY) {
			printk("audio_i2s: read failed (%d: %s)\n", ret, strerror(-ret));
			audio_i2s_stop();
			k_sleep(K_MSEC(AUDIO_I2S_RESTART_BACKOFF_MS));
		}
	}
}

bool audio_i2s_is_ready(void)
{
	return audio_i2s_ready;
}

int audio_i2s_tx_enqueue_block(const struct audio_i2s_block *block, k_timeout_t timeout)
{
	if (block == NULL) {
		return -EINVAL;
	}

	if (!audio_i2s_ready) {
		return -EAGAIN;
	}

	return k_msgq_put(&audio_i2s_tx_msgq, block, timeout);
}

int audio_i2s_rx_dequeue_block(struct audio_i2s_block *block, k_timeout_t timeout)
{
	if (block == NULL) {
		return -EINVAL;
	}

	if (!audio_i2s_ready) {
		return -EAGAIN;
	}

	return k_msgq_get(&audio_i2s_rx_msgq, block, timeout);
}

void audio_i2s_tx_flush(void)
{
	k_msgq_purge(&audio_i2s_tx_msgq);
}

void audio_i2s_rx_flush(void)
{
	k_msgq_purge(&audio_i2s_rx_msgq);
}

K_THREAD_DEFINE(audio_i2s_thread_id, AUDIO_I2S_THREAD_STACK_SIZE, audio_i2s_thread,
		NULL, NULL, NULL, AUDIO_I2S_THREAD_PRIORITY, 0, 0);
