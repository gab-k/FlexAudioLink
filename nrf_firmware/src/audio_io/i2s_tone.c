#include "audio_io/i2s_tone.h"

#include "audio_io/i2s.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define AUDIO_I2S_TONE_THREAD_STACK_SIZE 1536
#define AUDIO_I2S_TONE_THREAD_PRIORITY   9
#define AUDIO_I2S_TONE_IDLE_BACKOFF_MS   20

/* 1 kHz sine at 48 kHz = 48 samples/period, ~-0.3 dBFS peak. */
static const int16_t audio_i2s_tone_sine_lut[48] = {
	0,      4177,   8282,   12246,  16000,  19480,  22627,  25387,
	27713,  29564,  30910,  31726,  32000,  31726,  30910,  29564,
	27713,  25387,  22627,  19480,  16000,  12246,  8282,   4177,
	0,      -4177,  -8282,  -12246, -16000, -19480, -22627, -25387,
	-27713, -29564, -30910, -31726, -32000, -31726, -30910, -29564,
	-27713, -25387, -22627, -19480, -16000, -12246, -8282,  -4177,
};

static atomic_t g_audio_i2s_tone_enabled = ATOMIC_INIT(0);
static atomic_t g_audio_i2s_tone_reset_pending = ATOMIC_INIT(0);
static atomic_t g_audio_i2s_tone_enqueued_blocks = ATOMIC_INIT(0);
static uint32_t g_audio_i2s_tone_stereo_sample_index;

static void audio_i2s_tone_fill_block(struct audio_i2s_block *block)
{
	const uint32_t period_stereo_samples = ARRAY_SIZE(audio_i2s_tone_sine_lut);

	for (size_t stereo_sample = 0; stereo_sample < AUDIO_I2S_STEREO_SAMPLES_PER_BLOCK;
	     ++stereo_sample) {
		int16_t sample = audio_i2s_tone_sine_lut[g_audio_i2s_tone_stereo_sample_index %
							 period_stereo_samples];

		block->pcm16[(stereo_sample * AUDIO_I2S_CHANNELS) + 0U] = sample;
		block->pcm16[(stereo_sample * AUDIO_I2S_CHANNELS) + 1U] = sample;
		g_audio_i2s_tone_stereo_sample_index++;
	}
}

bool audio_i2s_tone_is_enabled(void)
{
	return atomic_get(&g_audio_i2s_tone_enabled) != 0;
}

void audio_i2s_tone_set_enabled(bool enabled)
{
	atomic_set(&g_audio_i2s_tone_enabled, enabled ? 1 : 0);
	atomic_set(&g_audio_i2s_tone_reset_pending, 1);
	atomic_set(&g_audio_i2s_tone_enqueued_blocks, 0);
	audio_i2s_tx_flush();
}

uint32_t audio_i2s_tone_get_enqueued_blocks(void)
{
	return (uint32_t)atomic_get(&g_audio_i2s_tone_enqueued_blocks);
}

static void audio_i2s_tone_thread(void *arg1, void *arg2, void *arg3)
{
	int ret;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		struct audio_i2s_block block;

		if (atomic_set(&g_audio_i2s_tone_reset_pending, 0) != 0) {
			g_audio_i2s_tone_stereo_sample_index = 0U;
		}

		if (!audio_i2s_tone_is_enabled() || !audio_i2s_is_ready()) {
			k_sleep(K_MSEC(AUDIO_I2S_TONE_IDLE_BACKOFF_MS));
			continue;
		}

		audio_i2s_tone_fill_block(&block);
		ret = audio_i2s_tx_enqueue_block(&block, K_MSEC(20));
		if (ret == 0) {
			atomic_inc(&g_audio_i2s_tone_enqueued_blocks);
		}
		if (ret < 0 && ret != -EAGAIN) {
			k_sleep(K_MSEC(AUDIO_I2S_TONE_IDLE_BACKOFF_MS));
		}
	}
}

K_THREAD_DEFINE(audio_i2s_tone_thread_id, AUDIO_I2S_TONE_THREAD_STACK_SIZE, audio_i2s_tone_thread,
		NULL, NULL, NULL, AUDIO_I2S_TONE_THREAD_PRIORITY, 0, 0);
