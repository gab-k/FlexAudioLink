#include "audio.h"
#include "main.h"
#include "stm32h7rsxx_hal_def.h"
#include "stm32h7rsxx_hal_dma_ex.h"
#include "stm32h7rsxx_hal_i2s.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "i2s.h"                // Access to hi2s2
#include "stm32h7rsxx_nucleo.h" // Access to BSP_PB, LEDs, etc


// List of supported sample rates
const uint32_t sample_rates[] = {44100, 48000};
uint32_t current_sample_rate = 48000;
#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

/* Blink pattern
 * - 25 ms   : streaming data
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_STREAMING = 25,
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

enum {
  VOLUME_CTRL_0_DB = 0,
  VOLUME_CTRL_10_DB = 2560,
  VOLUME_CTRL_20_DB = 5120,
  VOLUME_CTRL_30_DB = 7680,
  VOLUME_CTRL_40_DB = 10240,
  VOLUME_CTRL_50_DB = 12800,
  VOLUME_CTRL_60_DB = 15360,
  VOLUME_CTRL_70_DB = 17920,
  VOLUME_CTRL_80_DB = 20480,
  VOLUME_CTRL_90_DB = 23040,
  VOLUME_CTRL_100_DB = 25600,
  VOLUME_CTRL_SILENCE = 0x8000,
};

uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];   // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1]; // +1 for master channel 0

// Pointer to microphone data buffer
tu_fifo_t * mic_ff_ptr = NULL;
tu_fifo_buffer_info_t mic_ff_info;

// Pointer to speaker data buffer
tu_fifo_t * spk_ff_ptr = NULL;
tu_fifo_buffer_info_t spk_ff_info;


// Speaker data size received in the last frame
int spk_data_size;
// Resolution per format
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX};
// Current resolution, update on format change
uint8_t current_resolution;


//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

//--------------------------------------------------------------------+
// UAC1 Helper Functions
//--------------------------------------------------------------------+

static bool audio10_set_req_ep(tusb_control_request_t const *p_request, uint8_t *pBuff) {
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

  switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
      if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
        // Request uses 3 bytes
        TU_VERIFY(p_request->wLength == 3);

        current_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;

        TU_LOG2("EP set current freq: %" PRIu32 "\r\n", current_sample_rate);

        return true;
      }
      break;

    // Unknown/Unsupported control
    default:
      TU_BREAKPOINT();
      return false;
  }

  return false;
}

static bool audio10_get_req_ep(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

  switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
      if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        TU_LOG2("EP get current freq\r\n");

        uint8_t freq[3];
        freq[0] = (uint8_t) (current_sample_rate & 0xFF);
        freq[1] = (uint8_t) ((current_sample_rate >> 8) & 0xFF);
        freq[2] = (uint8_t) ((current_sample_rate >> 16) & 0xFF);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
      }
      break;

    // Unknown/Unsupported control
    default:
      TU_BREAKPOINT();
      return false;
  }

  return false;
}

static bool audio10_set_req_entity(tusb_control_request_t const *p_request, uint8_t *pBuff) {
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // If request is for our speaker feature unit
  if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
    switch (ctrlSel) {
      case AUDIO10_FU_CTRL_MUTE:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_SET_CUR:
            // Only 1st form is supported
            TU_VERIFY(p_request->wLength == 1);

            mute[channelNum] = pBuff[0];

            TU_LOG2("    Set Mute: %d of channel: %u\r\n", mute[channelNum], channelNum);
            return true;

          default:
            return false; // not supported
        }

      case AUDIO10_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_SET_CUR:
            // Only 1st form is supported
            TU_VERIFY(p_request->wLength == 2);

            volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff) / 256;

            TU_LOG2("    Set Volume: %d dB of channel: %u\r\n", volume[channelNum], channelNum);
            return true;

          default:
            return false; // not supported
        }

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // If request is for our speaker feature unit
  if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
    switch (ctrlSel) {
      case AUDIO10_FU_CTRL_MUTE:
        // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
        // There does not exist a range parameter block for mute
        TU_LOG2("    Get Mute of channel: %u\r\n", channelNum);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);

      case AUDIO10_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_GET_CUR:
            TU_LOG2("    Get Volume of channel: %u\r\n", channelNum);
            {
              int16_t vol = (int16_t) volume[channelNum];
              vol = vol * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
            }

          case AUDIO10_CS_REQ_GET_MIN:
            TU_LOG2("    Get Volume min of channel: %u\r\n", channelNum);
            {
              int16_t min = -90; // -90 dB
              min = min * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
            }

          case AUDIO10_CS_REQ_GET_MAX:
            TU_LOG2("    Get Volume max of channel: %u\r\n", channelNum);
            {
              int16_t max = 30; // +30 dB
              max = max * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
            }

          case AUDIO10_CS_REQ_GET_RES:
            TU_LOG2("    Get Volume res of channel: %u\r\n", channelNum);
            {
              int16_t res = 1; // 1 dB
              res = res * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
            }
            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  return false;
}

//--------------------------------------------------------------------+
// UAC2 Helper Functions
//--------------------------------------------------------------------+

#if TUD_OPT_HIGH_SPEED

// Helper for clock get requests
static bool audio20_clock_get_request(uint8_t rhport, audio20_control_request_t const *request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);

      audio20_control_cur_4_t curf = {(int32_t) tu_htole32(current_sample_rate)};
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &curf, sizeof(curf));
    } else if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_4_n_t(N_SAMPLE_RATES) rangef =
          {
              .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)};
      TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
      for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
        rangef.subrange[i].bMin = (int32_t) sample_rates[i];
        rangef.subrange[i].bMax = (int32_t) sample_rates[i];
        rangef.subrange[i].bRes = 0;
        TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int) rangef.subrange[i].bMin, (int) rangef.subrange[i].bMax, (int) rangef.subrange[i].bRes);
      }

      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &rangef, sizeof(rangef));
    }
  } else if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID &&
             request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t cur_valid = {.bCur = 1};
    TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &cur_valid, sizeof(cur_valid));
  }
  TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);
  return false;
}

// Helper for clock set requests
static bool audio20_clock_set_request(uint8_t rhport, audio20_control_request_t const *request, uint8_t const *buf) {
  (void) rhport;

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));

    current_sample_rate = (uint32_t) ((audio20_control_cur_4_t const *) buf)->bCur;

    TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);

    return true;
  } else {
    TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
}

// Helper for feature unit get requests
static bool audio20_feature_unit_get_request(uint8_t rhport, audio20_control_request_t const *request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t mute1 = {.bCur = mute[request->bChannelNumber]};
    TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &mute1, sizeof(mute1));
  } else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_2_n_t(1) range_vol = {
          .wNumSubRanges = tu_htole16(1),
          .subrange[0] = {.bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256)}};
      TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
              range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &range_vol, sizeof(range_vol));
    } else if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_2_t cur_vol = {.bCur = tu_htole16(volume[request->bChannelNumber])};
      TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &cur_vol, sizeof(cur_vol));
    }
  }
  TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);

  return false;
}

// Helper for feature unit set requests
static bool audio20_feature_unit_set_request(uint8_t rhport, audio20_control_request_t const *request, uint8_t const *buf) {
  (void) rhport;

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_1_t));

    mute[request->bChannelNumber] = ((audio20_control_cur_1_t const *) buf)->bCur;

    TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);

    return true;
  } else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_2_t));

    volume[request->bChannelNumber] = ((audio20_control_cur_2_t const *) buf)->bCur;

    TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber] / 256);

    return true;
  } else {
    TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
}

static bool audio20_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
  audio20_control_request_t const *request = (audio20_control_request_t const *) p_request;

  if (request->bEntityID == UAC2_ENTITY_CLOCK)
    return audio20_clock_get_request(rhport, request);
  if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
    return audio20_feature_unit_get_request(rhport, request);
  else {
    TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
  }
  return false;
}

static bool audio20_set_req_entity(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  audio20_control_request_t const *request = (audio20_control_request_t const *) p_request;

  if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
    return audio20_feature_unit_set_request(rhport, request, buf);
  if (request->bEntityID == UAC2_ENTITY_CLOCK)
    return audio20_clock_set_request(rhport, request, buf);
  TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);

  return false;
}

#endif // TUD_OPT_HIGH_SPEED

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  if (tud_audio_version() == 1) {
    return audio10_set_req_ep(p_request, pBuff);
  } else if (tud_audio_version() == 2) {
    // We do not support any requests here
  }

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_get_req_ep(rhport, p_request);
  } else if (tud_audio_version() == 2) {
    // We do not support any requests here
  }

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_get_req_entity(rhport, p_request);
#if TUD_OPT_HIGH_SPEED
  } else if (tud_audio_version() == 2) {
    return audio20_get_req_entity(rhport, p_request);
#endif
  }

  return false;
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_set_req_entity(p_request, buf);
#if TUD_OPT_HIGH_SPEED
  } else if (tud_audio_version() == 2) {
    return audio20_set_req_entity(rhport, p_request, buf);
#endif
  }

  return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

  if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt == 0) {
    blink_interval_ms = BLINK_MOUNTED;
  }

  return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

  TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
  if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0) {
    blink_interval_ms = BLINK_STREAMING;
    audio_init();
  }

  // Clear buffer when streaming format is changed
  spk_data_size = 0;
  if (alt != 0) {
    current_resolution = resolutions_per_format[alt - 1];
  }

  return true;
}

//--------------------------------------------------------------------+
// AUDIO Task
//--------------------------------------------------------------------+

void audio_task(void) {
  static bool started = false;
  uint16_t bytes_written;
  uint16_t available = tud_audio_available();
  if (!started) {
    if (available >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/4) {
      HAL_StatusTypeDef status = HAL_I2SEx_TransmitReceive_DMA(&hi2s2, (uint16_t*) spk_ff_ptr->buffer, (uint16_t*) mic_ff_ptr->buffer, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
      if (status != HAL_OK)
      {
        Error_Handler();
      }
      started = true;
      return;
    }
  }
}

void audio_control_task(void) {
  // Press on-board button to control volume
  // Open host volume control, volume should switch between 10% and 100%

  // Poll every 50ms
  const uint32_t interval_ms = 50;
  static uint32_t start_ms = 0;
  static uint32_t btn_prev = 0;

  if (HAL_GetTick() - start_ms < interval_ms) return;// not enough time
  start_ms += interval_ms;

  uint32_t btn = BSP_PB_GetState(BUTTON_USER);

  // Even UAC1 spec have status interrupt support like UAC2, most host do not support it
  // So you have to either use UAC2 or use old day HID volume control
  TU_VERIFY((tud_audio_version() == 1),);

  if (!btn_prev && btn) {
    // Adjust volume between 0dB (100%) and -30dB (10%)
    for (int i = 0; i < CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1; i++) {
      volume[i] = volume[i] == 0 ? -VOLUME_CTRL_30_DB : 0;
    }

    // 6.1 Interrupt Data Message
    const audio_interrupt_data_t data = {.v2 = {
        .bInfo = 0,                                      // Class-specific interrupt, originated from an interface
        .bAttribute = AUDIO20_CS_REQ_CUR,                // Caused by current settings
        .wValue_cn_or_mcn = 0,                           // CH0: master volume
        .wValue_cs = AUDIO20_FU_CTRL_VOLUME,             // Volume change
        .wIndex_ep_or_int = 0,                           // From the interface itself
        .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT,// From feature unit
    }};

    tud_audio_int_write(&data);
  }

  btn_prev = btn;
}


void audio_init() {
  spk_ff_ptr = tud_audio_get_ep_out_ff();
  mic_ff_ptr = tud_audio_get_ep_in_ff();
}

void audio_init_test(){
  init_test_sawtooth();
}

// void audio_start_test(){
//   HAL_StatusTypeDef status = HAL_I2SEx_TransmitReceive_DMA(&hi2s2, (uint16_t*) spk_intmdt_buf, (uint16_t*) mic_intmdt_buf, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
//   if (status != HAL_OK)
//   {
//     Error_Handler();
//   }
// }

// void init_test_sine_array() {
//   // Desired Frequency
//   float frequency = 12000.0f; // 12 kHz
//   // Sampling Rate
//   float sample_rate = 48000.0f; // 48 kHz
//   // Calculate the angle step for each sample to generate the desired frequency.
//   double angle_step = 2.0 * M_PI * frequency / sample_rate;
//     // Iterate through the buffer and fill it with sine data.
//   // We step by 2 because we are filling a stereo buffer (Left and Right channels).
//   for (unsigned int i = 0; i < sizeof(spk_intmdt_buf)/sizeof(spk_intmdt_buf[0]); i += 2) {
//     // Calculate the sine value for the current sample position.
//     // The sample index 'n' is i/2 because we handle L/R pairs.
//     int n = i / 2;
//     double sin_value = sin((double)n * angle_step);

//     // Scale the sine value by the amplitude and cast to a 16-bit integer.
//     int16_t sample_value = (int16_t)(28000 * sin_value);

//     // Place the same sample in both Left and Right channels.
//     spk_intmdt_buf[i]     = sample_value; // Left Channel
//     spk_intmdt_buf[i + 1] = sample_value; // Right Channel
//   }
//   // Clean the specific memory region of the buffer from the D-Cache
//   //SCB_CleanDCache_by_Addr((uint32_t*)test_spk_buf, sizeof(test_spk_buf));
// }


// void init_test_sawtooth() {
//   // --- Configuration ---
//   const int16_t amplitude = 28000;
//   const unsigned int total_samples = sizeof(spk_intmdt_buf) / sizeof(spk_intmdt_buf[0]);
  
//   // The number of L/R sample pairs in the buffer.
//   const unsigned int num_stereo_samples = total_samples / 2;

//   // --- Generate the Sawtooth Wave ---
//   for (unsigned int i = 0; i < num_stereo_samples; i++) {
//     // Calculate the value for the current sample.
//     // This creates a linear ramp from -amplitude to +amplitude.
//     double normalized_position = (double)i / (num_stereo_samples - 1); // Goes from 0.0 to 1.0
//     int16_t sample_value = (int16_t)(-amplitude + (2.0 * amplitude * normalized_position));

//     // Place the same sample in both Left and Right channels.
//     spk_intmdt_buf[i * 2]     = sample_value; // Left Channel
//     spk_intmdt_buf[i * 2 + 1] = sample_value; // Right Channel
//   }
  
//   // Clean the D-Cache once after modifying the entire buffer.
//   //SCB_CleanDCache_by_Addr((uint32_t*)test_spk_buf, sizeof(test_spk_buf));
// }


//--------------------------------------------------------------------+
// DMA Callbacks
//--------------------------------------------------------------------+

void I2S_DMA_TX_HalfCpltCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(TESTA_GPIO_Port, TESTA_Pin, GPIO_PIN_SET);
  tu_fifo_get_read_info(spk_ff_ptr, &spk_ff_info);
  tu_fifo_advance_read_pointer(spk_ff_ptr, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
  tu_fifo_get_read_info(spk_ff_ptr, &spk_ff_info);
  HAL_GPIO_WritePin(TESTA_GPIO_Port, TESTA_Pin, GPIO_PIN_RESET);

  // uint16_t bytes_written = tud_audio_read(spk_intmdt_buf, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
  // if(bytes_written < CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2)
  // {
  //   Error_Handler();
  // }
  // tu_fifo_get_read_info(spk_ff_ptr, &spk_ff_info);
  // uint16_t available = tud_audio_available();
  // HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_RESET);
  // available = tud_audio_available();
  // if(!available){
  //   Error_Handler();
  // }
}

void I2S_DMA_TX_CpltCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(TESTA_GPIO_Port, TESTA_Pin, GPIO_PIN_SET);
  tu_fifo_get_read_info(spk_ff_ptr, &spk_ff_info);
  tu_fifo_advance_read_pointer(spk_ff_ptr, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
  tu_fifo_get_read_info(spk_ff_ptr, &spk_ff_info);
  HAL_GPIO_WritePin(TESTA_GPIO_Port, TESTA_Pin, GPIO_PIN_RESET);
  //uint16_t available = tud_audio_available();
  //uint16_t bytes_written = tud_audio_read(&(spk_intmdt_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2]), CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2);
  // if(bytes_written < CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ/2)
  // {
  //   Error_Handler();
  // }
  // HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_SET);
  // uint16_t available = tud_audio_available();
  // if(!available){
  //   Error_Handler();
  // }
}

void I2S_DMA_TX_ErrorCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);
  // Handle the error appropriately
  Error_Handler();
}

void I2S_DMA_RX_HalfCpltCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_RESET);
}

void I2S_DMA_RX_CpltCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_SET);
}

void I2S_DMA_RX_ErrorCallback(DMA_HandleTypeDef *hdma)
{
  UNUSED(hdma);
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);
  // Handle the error appropriately
  Error_Handler();
}