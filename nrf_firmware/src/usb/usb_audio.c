#include "usb/usb_audio.h"

#include <stdbool.h>

#include "tusb.h"
#include "usb/usb_descriptors.h"

#define USB_AUDIO_FEEDBACK_FP_SHIFT        16U
#define USB_AUDIO_SPK_CHANNEL_COUNT        3U /* master + L + R */
#define USB_AUDIO_VOLUME_MIN_256DB         ((int16_t)(-50 * 256))
#define USB_AUDIO_VOLUME_MAX_256DB         ((int16_t)(0))
#define USB_AUDIO_VOLUME_RES_256DB         ((uint16_t)(1 * 256))

static int8_t g_usb_audio_speaker_mute[USB_AUDIO_SPK_CHANNEL_COUNT];
static int16_t g_usb_audio_speaker_volume[USB_AUDIO_SPK_CHANNEL_COUNT];

static void usb_audio_set_nominal_feedback(void)
{
	(void)tud_audio_fb_set(
		(uint32_t)(((uint64_t)CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
			    << USB_AUDIO_FEEDBACK_FP_SHIFT) / 8000ULL));
}

static bool usb_audio_uac2_get_clock_req(uint8_t rhport, audio20_control_request_t const *request)
{
	if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
		if (request->bRequest == AUDIO20_CS_REQ_CUR) {
			audio20_control_cur_4_t cur = {
				.bCur = (int32_t)tu_htole32(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE),
			};

			return tud_audio_buffer_and_schedule_control_xfer(
				rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
		}

		if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
			audio20_control_range_4_n_t(1) range = {
				.wNumSubRanges = tu_htole16(1U),
			};

			range.subrange[0].bMin =
				(int32_t)tu_htole32(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE);
			range.subrange[0].bMax =
				(int32_t)tu_htole32(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE);
			range.subrange[0].bRes = tu_htole32(0U);

			return tud_audio_buffer_and_schedule_control_xfer(
				rhport, (tusb_control_request_t const *)request, &range, sizeof(range));
		}
	}

	if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID &&
	    request->bRequest == AUDIO20_CS_REQ_CUR) {
		audio20_control_cur_1_t cur = {
			.bCur = 1,
		};

		return tud_audio_buffer_and_schedule_control_xfer(
			rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
	}

	return false;
}

static bool usb_audio_uac2_set_clock_req(audio20_control_request_t const *request, uint8_t const *buf)
{
	uint16_t req_len;
	uint32_t requested_rate_hz;

	if (request->bRequest != AUDIO20_CS_REQ_CUR ||
	    request->bControlSelector != AUDIO20_CS_CTRL_SAM_FREQ) {
		return false;
	}

	req_len = tu_le16toh(request->wLength);
	if (req_len != sizeof(audio20_control_cur_4_t)) {
		return false;
	}

	requested_rate_hz = tu_le32toh((uint32_t)((audio20_control_cur_4_t const *)buf)->bCur);
	if (requested_rate_hz != CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE) {
		return false;
	}

	return true;
}

static bool usb_audio_uac2_get_feature_req(uint8_t rhport, audio20_control_request_t const *request)
{
	uint8_t channel;

	channel = request->bChannelNumber;
	if (channel >= USB_AUDIO_SPK_CHANNEL_COUNT) {
		return false;
	}

	if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE &&
	    request->bRequest == AUDIO20_CS_REQ_CUR) {
		audio20_control_cur_1_t cur = {
			.bCur = g_usb_audio_speaker_mute[channel],
		};

		return tud_audio_buffer_and_schedule_control_xfer(
			rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
	}

	if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
		if (request->bRequest == AUDIO20_CS_REQ_CUR) {
			audio20_control_cur_2_t cur = {
				.bCur = tu_htole16(g_usb_audio_speaker_volume[channel]),
			};

			return tud_audio_buffer_and_schedule_control_xfer(
				rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
		}

		if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
			audio20_control_range_2_n_t(1) range = {
				.wNumSubRanges = tu_htole16(1U),
			};

			range.subrange[0].bMin = tu_htole16(USB_AUDIO_VOLUME_MIN_256DB);
			range.subrange[0].bMax = tu_htole16(USB_AUDIO_VOLUME_MAX_256DB);
			range.subrange[0].bRes = tu_htole16(USB_AUDIO_VOLUME_RES_256DB);

			return tud_audio_buffer_and_schedule_control_xfer(
				rhport, (tusb_control_request_t const *)request, &range, sizeof(range));
		}
	}

	return false;
}

static bool usb_audio_uac2_set_feature_req(audio20_control_request_t const *request, uint8_t const *buf)
{
	uint8_t channel;
	uint16_t req_len;

	channel = request->bChannelNumber;
	if (channel >= USB_AUDIO_SPK_CHANNEL_COUNT) {
		return false;
	}

	if (request->bRequest != AUDIO20_CS_REQ_CUR) {
		return false;
	}

	if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
		req_len = tu_le16toh(request->wLength);
		if (req_len != sizeof(audio20_control_cur_1_t)) {
			return false;
		}

		g_usb_audio_speaker_mute[channel] =
			((audio20_control_cur_1_t const *)buf)->bCur ? 1 : 0;
		return true;
	}

	if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
		int16_t volume_db_256;

		req_len = tu_le16toh(request->wLength);
		if (req_len != sizeof(audio20_control_cur_2_t)) {
			return false;
		}

		volume_db_256 =
			(int16_t)tu_le16toh((uint16_t)((audio20_control_cur_2_t const *)buf)->bCur);
		if (volume_db_256 < USB_AUDIO_VOLUME_MIN_256DB ||
		    volume_db_256 > USB_AUDIO_VOLUME_MAX_256DB) {
			return false;
		}

		g_usb_audio_speaker_volume[channel] = volume_db_256;
		return true;
	}

	return false;
}

void usb_audio_reset(void)
{
	tu_fifo_t *ep_out_ff;
	tu_fifo_t *ep_in_ff;

	for (size_t channel = 0U; channel < USB_AUDIO_SPK_CHANNEL_COUNT; channel++) {
		g_usb_audio_speaker_mute[channel] = 0;
		g_usb_audio_speaker_volume[channel] = USB_AUDIO_VOLUME_MAX_256DB;
	}

	usb_audio_set_nominal_feedback();

	ep_out_ff = tud_audio_get_ep_out_ff();
	ep_in_ff = tud_audio_get_ep_in_ff();

	if (ep_out_ff != NULL) {
		tu_fifo_clear(ep_out_ff);
	}
	if (ep_in_ff != NULL) {
		tu_fifo_clear(ep_in_ff);
	}
}

size_t usb_audio_write_microphone_bytes(const uint8_t *data, size_t bytes)
{
	tu_fifo_t *ep_in_ff;
	uint16_t space;
	uint16_t want;

	if (data == NULL || bytes == 0U) {
		return 0U;
	}

	ep_in_ff = tud_audio_get_ep_in_ff();
	if (ep_in_ff == NULL) {
		return 0U;
	}

	space = tu_fifo_remaining(ep_in_ff);
	if (space == 0U) {
		return 0U;
	}

	want = (bytes > UINT16_MAX) ? UINT16_MAX : (uint16_t)bytes;
	if (want > space) {
		want = space;
	}

	return tu_fifo_write_n(ep_in_ff, data, want);
}

uint32_t usb_audio_microphone_level_bytes(void)
{
	tu_fifo_t *ep_in_ff = tud_audio_get_ep_in_ff();

	if (ep_in_ff == NULL) {
		return 0U;
	}

	return tu_fifo_count(ep_in_ff);
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
	audio20_control_request_t const *request;

	if (p_request == NULL || tud_audio_version() != 2U) {
		return false;
	}

	request = (audio20_control_request_t const *)p_request;

	switch (request->bEntityID) {
	case UAC2_ENTITY_CLOCK:
		return usb_audio_uac2_get_clock_req(rhport, request);
	case UAC2_ENTITY_SPK_FEATURE_UNIT:
		return usb_audio_uac2_get_feature_req(rhport, request);
	default:
		return false;
	}
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request,
				 uint8_t *buf)
{
	audio20_control_request_t const *request;

	(void)rhport;

	if (p_request == NULL || buf == NULL || tud_audio_version() != 2U) {
		return false;
	}

	request = (audio20_control_request_t const *)p_request;

	switch (request->bEntityID) {
	case UAC2_ENTITY_CLOCK:
		return usb_audio_uac2_set_clock_req(request, buf);
	case UAC2_ENTITY_SPK_FEATURE_UNIT:
		return usb_audio_uac2_set_feature_req(request, buf);
	default:
		return false;
	}
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
	tu_fifo_t *ff;
	uint8_t itf;
	uint8_t alt;

	(void)rhport;

	if (p_request == NULL) {
		return true;
	}

	itf = tu_u16_low(tu_le16toh(p_request->wIndex));
	alt = tu_u16_low(tu_le16toh(p_request->wValue));

	if (itf == ITF_NUM_AUDIO_STREAMING_SPK && alt == 0U) {
		usb_audio_set_nominal_feedback();
		ff = tud_audio_get_ep_out_ff();
		if (ff != NULL) {
			tu_fifo_clear(ff);
		}
	}

	if (itf == ITF_NUM_AUDIO_STREAMING_MIC && alt == 0U) {
		ff = tud_audio_get_ep_in_ff();
		if (ff != NULL) {
			tu_fifo_clear(ff);
		}
	}

	return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
	uint8_t itf;
	uint8_t alt;

	(void)rhport;

	if (p_request == NULL) {
		return true;
	}

	itf = tu_u16_low(tu_le16toh(p_request->wIndex));
	alt = tu_u16_low(tu_le16toh(p_request->wValue));

	if (itf == ITF_NUM_AUDIO_STREAMING_SPK && alt != 0U) {
		usb_audio_set_nominal_feedback();
	}

	return true;
}
