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
#include "gpdma.h"
#include "i2s.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdbool.h"
#include "pll.h"
#include <stdint.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "audio.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
extern uint32_t blink_interval_ms;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SAMPLES_TEST_ARRAY 8192
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

/* USER CODE BEGIN PV */

extern DMA_QListTypeDef I2S2_TX_Queue;
extern DMA_QListTypeDef I2S2_RX_Queue;


// Calculate the total number of samples in the test buffer.
volatile int16_t test_spk_buf[SAMPLES_TEST_ARRAY*2];
volatile int16_t rx_buf[SAMPLES_TEST_ARRAY*2];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MPU_Config(void);
/* USER CODE BEGIN PFP */


void led_blinking_task(void);
void audio_task(void);
void audio_control_task(void);
void set_debugmcu_bits(void);

void i2s_playback_blocking(void);
void i2s_playback_dma(void);
void init_test_sine_array(void);
void init_test_sawtooth(void);
void dummy_entry_function(void);
inline void dummy_entry_function(void) {
  // This function is intentionally left empty.
  return;
}
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
  dummy_entry_function();
  
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

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
  MX_GPDMA1_Init();
  MX_USB_OTG_HS_PCD_Init();
  MX_I2S2_Init();
  /* USER CODE BEGIN 2 */

  // Used for debugging
  enable_dwt_cycle_counter();
  set_debugmcu_bits();

  // Init tinyUSB device stack
  tusb_rhport_init_t dev_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  
  Init_I2S2_TX_DMA_Queue(&handle_GPDMA1_Channel0, &I2S2_TX_Queue);
  Init_I2S2_RX_DMA_Queue(&handle_GPDMA1_Channel1, &I2S2_RX_Queue);

  init_test_sawtooth();


  
  /* USER CODE END 2 */

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
  
  i2s_playback_dma();
  
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    // TinyUSB device task handles USB stack & TinyUSB callbacks
    tud_task();
    //audio_task();
    audio_control_task();
    led_blinking_task();
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

void init_test_sine_array() {
  // Desired Frequency
  float frequency = 12000.0f; // 12 kHz
  // Sampling Rate
  float sample_rate = 48000.0f; // 48 kHz
  // Calculate the angle step for each sample to generate the desired frequency.
  double angle_step = 2.0 * M_PI * frequency / sample_rate;
    // Iterate through the buffer and fill it with sine data.
  // We step by 2 because we are filling a stereo buffer (Left and Right channels).
  for (unsigned int i = 0; i < sizeof(test_spk_buf)/sizeof(test_spk_buf[0]); i += 2) {
    // Calculate the sine value for the current sample position.
    // The sample index 'n' is i/2 because we handle L/R pairs.
    int n = i / 2;
    double sin_value = sin((double)n * angle_step);

    // Scale the sine value by the amplitude and cast to a 16-bit integer.
    int16_t sample_value = (int16_t)(28000 * sin_value);

    // Place the same sample in both Left and Right channels.
    test_spk_buf[i]     = sample_value; // Left Channel
    test_spk_buf[i + 1] = sample_value; // Right Channel
  }
  // Clean the specific memory region of the buffer from the D-Cache
  SCB_CleanDCache_by_Addr((uint32_t*)test_spk_buf, sizeof(test_spk_buf));
}


void init_test_sawtooth() {
  // --- Configuration ---
  const int16_t amplitude = 28000;
  const unsigned int total_samples = sizeof(test_spk_buf) / sizeof(test_spk_buf[0]);
  
  // The number of L/R sample pairs in the buffer.
  const unsigned int num_stereo_samples = total_samples / 2;

  // --- Generate the Sawtooth Wave ---
  for (unsigned int i = 0; i < num_stereo_samples; i++) {
    // Calculate the value for the current sample.
    // This creates a linear ramp from -amplitude to +amplitude.
    double normalized_position = (double)i / (num_stereo_samples - 1); // Goes from 0.0 to 1.0
    int16_t sample_value = (int16_t)(-amplitude + (2.0 * amplitude * normalized_position));

    // Place the same sample in both Left and Right channels.
    test_spk_buf[i * 2]     = sample_value; // Left Channel
    test_spk_buf[i * 2 + 1] = sample_value; // Right Channel
  }
  
  // Clean the D-Cache once after modifying the entire buffer.
  SCB_CleanDCache_by_Addr((uint32_t*)test_spk_buf, sizeof(test_spk_buf));
}


void i2s_playback_blocking() {
  HAL_StatusTypeDef status = HAL_I2S_Transmit(&hi2s2, (uint16_t*)test_spk_buf, sizeof(test_spk_buf)/sizeof(test_spk_buf[0]),400);
  if (status != HAL_OK)
  {
    
      Error_Handler();
  }
}

void i2s_playback_dma() {
  HAL_StatusTypeDef status = HAL_I2SEx_TransmitReceive_DMA(&hi2s2, (uint16_t*)test_spk_buf, (uint16_t*) rx_buf, sizeof(test_spk_buf)/sizeof(test_spk_buf[0]));
  if (status != HAL_OK)
  {
      Error_Handler();
  }
}



void set_debugmcu_bits(void)
{
  // Make DMA stop on debug
  DBGMCU->AHB1FZR |= DBGMCU_AHB1FZR_GPDMA_0_Msk;
}

// Enable DWT cycle counter
void enable_dwt_cycle_counter(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // Enable DWT
  DWT->CYCCNT = 0;                                // Reset counter
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;            // start counter
}

// Returns cycle difference between end and start cycle counts.
// The end parameter should be the counter which is captured later.
uint32_t get_dwt_cycle_difference(uint32_t end, uint32_t start)
{
  return (end - start);
}

// Returns DWT cycle counter; 1 cycle is one CPU cycle with the duration of 1/f_cpuclock
uint32_t get_dwt_cycle_counter()
{
  return DWT->CYCCNT;
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

 /* MPU Configuration */

static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /* Disables all MPU regions */
  for(uint8_t i=0; i<__MPU_REGIONCOUNT; i++)
  {
    HAL_MPU_DisableRegion(i);
  }

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x24067000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x24067000 + 0x8000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8KB;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  BSP_LED_On(LED_RED);
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
