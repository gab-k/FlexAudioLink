/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stm32h7rsxx_hal.h"
#include "stm32h7rsxx_nucleo.h"
#include "tusb.h"
#include "usb_descriptors.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

I2S_HandleTypeDef hi2s1;

PCD_HandleTypeDef hpcd_USB_OTG_HS;

/* USER CODE BEGIN PV */

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

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];   // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1]; // +1 for master channel 0

// Buffer for microphone data
int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];
// Buffer for speaker data
int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
// Speaker data size received in the last frame
int spk_data_size;
// Resolution per format
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
// Current resolution, update on format change
uint8_t current_resolution;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GPIO_Init(void);
static void MX_USB_OTG_HS_PCD_Init(void);
static void MX_I2S1_Init(void);
/* USER CODE BEGIN PFP */
void led_blinking_task(void);
void audio_task(void);
void audio_control_task(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Update SystemCoreClock variable according to RCC registers values. */
  SystemCoreClockUpdate();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USB_OTG_HS_PCD_Init();
  MX_I2S1_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_BLUE);
  BSP_LED_Init(LED_RED);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  
  // init device stack on configured roothub port
  tusb_rhport_init_t dev_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    tud_task();// TinyUSB device task
    audio_task();
    audio_control_task();
    led_blinking_task();
  }
  /* USER CODE END 3 */
}

/**
  * @brief I2S1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2S1_Init(void)
{

  /* USER CODE BEGIN I2S1_Init 0 */

  /* USER CODE END I2S1_Init 0 */

  /* USER CODE BEGIN I2S1_Init 1 */

  /* USER CODE END I2S1_Init 1 */
  hi2s1.Instance = SPI1;
  hi2s1.Init.Mode = I2S_MODE_MASTER_FULLDUPLEX;
  hi2s1.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s1.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s1.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
  hi2s1.Init.AudioFreq = I2S_AUDIOFREQ_48K;
  hi2s1.Init.CPOL = I2S_CPOL_LOW;
  hi2s1.Init.FirstBit = I2S_FIRSTBIT_MSB;
  hi2s1.Init.WSInversion = I2S_WS_INVERSION_DISABLE;
  hi2s1.Init.Data24BitAlignment = I2S_DATA_24BIT_ALIGNMENT_RIGHT;
  hi2s1.Init.MasterKeepIOState = I2S_MASTER_KEEP_IO_STATE_DISABLE;
  if (HAL_I2S_Init(&hi2s1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2S1_Init 2 */

  /* USER CODE END I2S1_Init 2 */

}

/**
  * @brief USB_OTG_HS Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_OTG_HS_PCD_Init(void)
{

  /* USER CODE BEGIN USB_OTG_HS_PCD_Init 0 */

  /* USER CODE END USB_OTG_HS_PCD_Init 0 */

  /* USER CODE BEGIN USB_OTG_HS_PCD_Init 1 */

  /* USER CODE END USB_OTG_HS_PCD_Init 1 */
  hpcd_USB_OTG_HS.Instance = USB_OTG_HS;
  hpcd_USB_OTG_HS.Init.dev_endpoints = 9;
  hpcd_USB_OTG_HS.Init.speed = PCD_SPEED_HIGH;
  hpcd_USB_OTG_HS.Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY;
  hpcd_USB_OTG_HS.Init.dma_enable = ENABLE;
  hpcd_USB_OTG_HS.Init.Sof_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_HS.Init.use_dedicated_ep1 = DISABLE;
  hpcd_USB_OTG_HS.Init.vbus_sensing_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_HS) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_OTG_HS_PCD_Init 2 */

  /* USER CODE END USB_OTG_HS_PCD_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

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

// This task simulates an audio transfer callback, one frame is sent/received every 1ms.
// In a real application, this would be replaced with actual I2S send/receive callback.
void audio_task(void) {
  static uint32_t start_ms = 0;
  uint32_t curr_ms = HAL_GetTick();
  if (start_ms == curr_ms) return;// not enough time
  start_ms = curr_ms;
  // When new data arrived, copy data from speaker buffer, to microphone buffer
  // and send it over
  // Only support speaker & headphone both have the same resolution
  // If one is 16bit another is 24bit be care of LOUD noise !
  spk_data_size = tud_audio_read(spk_buf, sizeof(spk_buf));
  if (spk_data_size) {
    if (current_resolution == 16) {
      int16_t *src = (int16_t *) spk_buf;
      int16_t *limit = (int16_t *) spk_buf + spk_data_size / 2;
      int16_t *dst = (int16_t *) mic_buf;
      while (src < limit) {
        // Combine two channels into one
        int32_t left = *src++;
        int32_t right = *src++;
        *dst++ = (int16_t) ((left >> 1) + (right >> 1));
      }
      tud_audio_write((uint8_t *) mic_buf, (uint16_t) (spk_data_size / 2));
      spk_data_size = 0;
    } else if (current_resolution == 24) {
      int32_t *src = spk_buf;
      int32_t *limit = spk_buf + spk_data_size / 4;
      int32_t *dst = mic_buf;
      while (src < limit) {
        // Combine two channels into one
        int32_t left = *src++;
        int32_t right = *src++;
        *dst++ = (int32_t) ((uint32_t) ((left >> 1) + (right >> 1)) & 0xffffff00ul);
      }
      tud_audio_write((uint8_t *) mic_buf, (uint16_t) (spk_data_size / 2));
      spk_data_size = 0;
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

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void) {
  static uint32_t start_ms = 0;
  static bool led_state = false;

  // Blink every interval ms
  if (HAL_GetTick()  - start_ms < blink_interval_ms) return;
  start_ms += blink_interval_ms;

  if (led_state){
    BSP_LED_On(LED_GREEN);
  }
  else{
    BSP_LED_Off(LED_GREEN);
  }
  led_state = 1 - led_state;
}


/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
