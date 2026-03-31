#include "audio.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "peripherals.h"
#include "mode.h"
#include "log.h"
#include "wlan.h"

// TODO: Seperate I2S related playback/record code and USB related audio callbacks into different files.

// ----------------------------------------------------------------+
// Audio Variables
// ----------------------------------------------------------------+
const uint32_t sample_rates[] = {48000};
uint32_t current_sample_rate = 48000;
#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)
#define AUDIO_DEBUG_LVL 0

/* Blink pattern
 * - 100 ms   : streaming data
 * - 500 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_STREAMING = 100, 
  BLINK_NOT_MOUNTED = 500,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};
uint32_t g_blink_interval_ms = BLINK_NOT_MOUNTED;

// Volume/Mute related audio controls (Not implemented yet)
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

// Current states
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];   // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1]; // +1 for master channel 0


// Resolution per format
const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX};
// Current resolution, update on format change
uint8_t current_resolution;


// --- Parallel Size Queue (Task -> ISR) ---
// Must be at least as deep as the I2S driver queue (usually 4)
#define SIZE_Q_DEPTH I2S_NUM_BUFFERS

// Min/Max Block sizes in Bytes 
#define MAX_DMA_BLOCK_SIZE (192*8) // 448  // Avoid one huge transfer, always have multiple scheduled
#define MIN_DMA_BLOCK_SIZE 192     // Avoid the overhead of many tiny DMA interrupts

volatile uint32_t dma_size_queue[SIZE_Q_DEPTH] = {0, 0, 0, 0};
volatile uint8_t  queue_wr_idx = 0;
volatile uint8_t  queue_rd_idx = 0;

typedef enum {
    AUDIO_STATE_BUFFERING,
    AUDIO_STATE_PLAYING
} audio_state_t;

// These are the active FIFOs used by the audio task and ISR
// Depending on mode these point to different FIFOs
// When reset or not initialized, they are NULL
static tu_fifo_t * spk_ff = NULL;
static tu_fifo_t * mic_ff = NULL;

// Audio state and buffer level
// Used by feedback controller and for state management inside audio task
volatile audio_state_t g_audio_state = AUDIO_STATE_BUFFERING;
volatile uint16_t g_audio_buf_lvl_filt_uint = 0;
volatile float g_audio_buf_lvl_filt_f = -1.0f; // -1 to detect first run

// --- External Handles from auto-generated code ---
// These are defined in peripherals.c
extern i2s_dma_handle_t FLEXCOMM0_Tx_DMA_Handle; 

// --- Task Handle ---
TaskHandle_t g_audio_task_handle = NULL;
TaskHandle_t g_audio_fb_task_handle = NULL;

// ----------------------------------------------------------------+
// Audio Task and Buffer Level Variables
// ----------------------------------------------------------------+

// --- Helpers ---
static void audio_feedback_forward_metric(uint32_t value_16_16);

// This calculates how much data is currently "owned" by the DMA
static inline uint16_t size_q_get_bytes_in_flight(void)
{
    uint16_t total = 0;
    for (int i = 0; i < SIZE_Q_DEPTH; i++) {
        total += dma_size_queue[i];
    }
    return total;
}

// Task calls this to save the xfer size of the data block submitted to DMA
static void size_q_push(uint16_t size) {
  // 1. Save size into queue
  dma_size_queue[queue_wr_idx] = size;
  // 2. Advance write index
  queue_wr_idx = (queue_wr_idx + 1) % SIZE_Q_DEPTH;
}

// Task calls this to undo the latest saved xfer size
static void size_q_undo_push(void) {
    queue_wr_idx = (queue_wr_idx + SIZE_Q_DEPTH - 1) % SIZE_Q_DEPTH;
    dma_size_queue[queue_wr_idx] = 0; // Zero out the abandoned slot
}

// ISR calls this to know the transfer size of the xfer that just finished
static uint32_t size_q_pop(void) {
  if (queue_wr_idx == queue_rd_idx){
    configASSERT(false); // Queue underflow
  } 
  // 1. Get size of finished transfer
  uint32_t size = dma_size_queue[queue_rd_idx];
  // 2. Clear size entry of finished transfer
  dma_size_queue[queue_rd_idx] = 0;
  // 3. Advance read index
  queue_rd_idx = (queue_rd_idx + 1) % SIZE_Q_DEPTH;
  return size;
}

static void size_q_clear(void) {
    queue_wr_idx = 0;
    queue_rd_idx = 0;
    for (int i = 0; i < SIZE_Q_DEPTH; i++) {
        dma_size_queue[i] = 0;
    }
}

bool q_full(void) {
    return ((queue_wr_idx + 1) % SIZE_Q_DEPTH) == queue_rd_idx;
}

void I2S_TX_DMA_Callback(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Retrieve and clear the size of the block that just finished.
    uint32_t finished_size = size_q_pop();

    // Check for NULL pointer
    if (spk_ff == NULL) {
      // No active speaker FIFO, should not happen.
      configASSERT(false); 
      return;
    }

    // Advance the read pointer/index of TinyUSB buffer.
    if (finished_size > 0) 
    {
      tu_fifo_advance_read_pointer(spk_ff, finished_size);
    }

    // Notify audio task, which queues more data if available.
    if (g_audio_task_handle != NULL) {
      vTaskNotifyGiveFromISR(g_audio_task_handle, &xHigherPriorityTaskWoken);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
        

// TODO: Rename into something clearer that this is used for playback/recording and CODEC related.
// TODO: Improve comments here, either the udp_rx/udp_tx or the tinyUSB callbacks write/read into
//       spk_ff/mic_ff respectively but this task doesnt need to know that.
void audio_task(void *pvParameters)
{  
  i2s_transfer_t xfer;
  uint16_t buf_level, flight;
  tu_fifo_buffer_info_t spk_ff_info;

  uint16_t fifo_depth;
  uint16_t start_threshold;

  const float ALPHA = 0.05f;
  

  for (;;)
  {
    // Wait for ISR notification or timeout
    uint32_t timeout_val_ms = 10;
    uint32_t notified_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_val_ms));
    //ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_val_ms));
    
    if (notified_value == 0) {
      // Woken by timeout!
      // CRITICAL GUARD: Only print this if when actively playing.
      // In BUFFERING state, no network packets are arriving...
      if (g_audio_state == AUDIO_STATE_PLAYING) {
          PRINTF("WARN: audio_task woken by %u ms timeout!\n", timeout_val_ms);
      }
    }

    // Detect and kill WiFi PS re-enabling
    {
      static bool ps_was_enabled = false;
      bool ps_enabled = wlan_is_power_save_enabled();
      if (ps_enabled && !ps_was_enabled)
        PRINTF("[AUD] WARN: WiFi PS re-enabled! Disabling...\r\n");
      ps_was_enabled = ps_enabled;
      if (ps_enabled) {
        wlan_ieeeps_off();
        wlan_deepsleepps_off();
      }
    }

    // Pass address of FIFO pointers, as its pass by-value in C
    get_active_fifos(&spk_ff, &mic_ff);

    // Speaker FIFO pointer will be NULL if not yet initialized or set to NULL due to mode switch
    if (spk_ff == NULL) {
      continue;
    }

    // Calculate thresholds based on the actual FIFO size they can differ depending on mode.
    fifo_depth = tu_fifo_depth(spk_ff);
    
    // Start playing when buffer is 50% full
    start_threshold = fifo_depth / 2; 

    // Keep queuing while there is data available
    while (1)
    {
      // 1. Check Queue Capacity (No Critical Section needed)
      //    It is safe to read queue_rd_idx here. 
      //    If ISR fires after this read, it only creates MORE space, which is safe.
      if (((queue_wr_idx + 1) % SIZE_Q_DEPTH) == queue_rd_idx) {
          // Queue is full. We cannot push more metadata.
          break;
      }

      // 2. Get buffer information inside critical section
      taskENTER_CRITICAL();
      // Total available bytes in speaker buffer
      buf_level = tu_fifo_count(spk_ff);
      // Total bytes currently "in flight" in the I2S DMA queue
      flight = size_q_get_bytes_in_flight();
      // Retrieve FIFO info which includes read pointers and lengths
      tu_fifo_get_read_info(spk_ff, &spk_ff_info);
      // Get in flight of active transfer.
      taskEXIT_CRITICAL();
      
      // Apply Moving Average (Low Pass Filter)
      if (g_audio_buf_lvl_filt_f < 0.0f) {
          g_audio_buf_lvl_filt_f = (float)buf_level; // Initialize instantly
      } else {
          g_audio_buf_lvl_filt_f = (ALPHA * (float)buf_level) + ((1.0f - ALPHA) * g_audio_buf_lvl_filt_f);
      }

      g_audio_buf_lvl_filt_uint = (uint16_t)g_audio_buf_lvl_filt_f;

      // 3. Handle states
      if (g_audio_state == AUDIO_STATE_BUFFERING) {
        if (buf_level >= start_threshold) {
          // Switch to PLAYING state if there is enough data  
          g_audio_state = AUDIO_STATE_PLAYING;
          PRINTF("Playing...  buf_lvl: %u \n", buf_level);
          //PRINTF("Playing... lvl: %u flight: %u sz_q: %u, %u, %u, %u\n", buf_level, flight, dma_size_queue[0], dma_size_queue[1], dma_size_queue[2], dma_size_queue[3]);
        }
        else {
          // Stay in BUFFERING state, exit loop
          break; 
        }
      } else if (g_audio_state == AUDIO_STATE_PLAYING) {
        if (buf_level == 0 && flight == 0) {
          // Switch to BUFFERING state if there is no data available
          g_audio_state = AUDIO_STATE_BUFFERING;
          g_audio_buf_lvl_filt_f = -1.0f; // Reset filter to be re-initialized on next run
          PRINTF("Buffering...\n");
          //PRINTF("Buffering... lvl: %u flight: %u sz_q: %u, %u, %u, %u\n", buf_level, flight, dma_size_queue[0], dma_size_queue[1], dma_size_queue[2], dma_size_queue[3]);
          // Also exit loop, because no data needs to be queued
          break;
        }
        // Warn on 90% Buffer fill state 
        // TODO: Possibly remove when finished with debugging
        else if(buf_level > (9*fifo_depth)/10) {
          PRINTF("W: B @ 90%%! g_lvl_filt: %u lvl: %u flight: %u sz_q: %u, %u, %u, %u\n", g_audio_buf_lvl_filt_uint, buf_level, flight, dma_size_queue[0], dma_size_queue[1], dma_size_queue[2], dma_size_queue[3]);
        }
        // Warn on 10% Buffer fill state
        // TODO: Possibly remove when finished with debugging
        else if(buf_level < fifo_depth/10) {
          PRINTF("W: B @ 10%%! g_lvl_filt: %u lvl: %u flight: %u sz_q: %u, %u, %u, %u\n", g_audio_buf_lvl_filt_uint, buf_level, flight, dma_size_queue[0], dma_size_queue[1], dma_size_queue[2], dma_size_queue[3]);
        }
      }

      // 4. Safety Checks
      // 'buf_level' is the total amount of data available in the TinyUSB buffer.
      // 'flight' is the amount of data currently queued to the I2S DMA queue (i2sQueue).
      // Therefore, (buf_level - flight) must always be >= 0.
      if (flight > buf_level) { 
        // Assert if flight exceeds available
        configASSERT(false); 
      }

      // 5. Calculate the amount of data pending to be queued to I2S DMA
      uint32_t virtual_avail = buf_level - flight;
      if (virtual_avail == 0) {
        break;
      }

      // We want to group data into larger blocks (MIN_DMA_BLOCK_SIZE).
      // BUT if the DMA is running dry (flight < MIN_DMA_BLOCK_SIZE), 
      // we must feed it whatever we have immediately to avoid deadlocks!
      if ((virtual_avail < MIN_DMA_BLOCK_SIZE) && (flight >= MIN_DMA_BLOCK_SIZE)) {
        break;
      }

      // 6. Calculate pointers and length of data to be queued
      uint8_t *dma_ptr;
      uint32_t dma_len;
      if (flight < spk_ff_info.linear.len) {
        // Next data block to be queued resides inside the linear part
        dma_ptr = spk_ff_info.linear.ptr + flight;
        dma_len = spk_ff_info.linear.len - flight;
      } else {
        // Next data block to be queued resides inside the wrapped part
        uint32_t offset = flight - spk_ff_info.linear.len;
        dma_ptr = spk_ff_info.wrapped.ptr + offset;
        dma_len = spk_ff_info.wrapped.len - offset;
      }

      // 7. Limit dma_len to max_dma_len
      if (dma_len > MAX_DMA_BLOCK_SIZE) dma_len = MAX_DMA_BLOCK_SIZE;

      // 8. Set transfer pointer and size
      xfer.data = dma_ptr;
      xfer.dataSize = dma_len;

      // 9. Add the size to queue, this needs to be done because the ISR could fire instantly.
      size_q_push(dma_len);

      // 10. Start DMA Transfer
      status_t status = I2S_TxTransferSendDMA(FLEXCOMM0_PERIPHERAL, &FLEXCOMM0_Tx_DMA_Handle, xfer);
      
      // 11. Check Status
      if (status != kStatus_Success) {
        // DMA submission failed, undo the latest push!
        size_q_undo_push();
        PRINTF("ERROR: %d DMA submission failed! i2sQueue: %u, %u, %u, %u\n",
                status,
                FLEXCOMM0_Tx_DMA_Handle.i2sQueue[0].dataSize,
                FLEXCOMM0_Tx_DMA_Handle.i2sQueue[1].dataSize,
                FLEXCOMM0_Tx_DMA_Handle.i2sQueue[2].dataSize,
                FLEXCOMM0_Tx_DMA_Handle.i2sQueue[3].dataSize);
        // Exit loop and wait for the next notification
        break;
      }
    }
  }
}  

void audio_fb_task(void *pvParameters)
{
  const TickType_t xFrequency = pdMS_TO_TICKS(25);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  const float KP = 0.05f;
  const float MAX_RATE_ADJUSTMENT = 100.0f;

  float current_sample_rate_f = (float)current_sample_rate;
  float proportional, adjusted_rate, samples_per_frame;
  uint32_t feedback_value;

  uint32_t dwt_last = DWT->CYCCNT;

  for (;;)
  {
    // Wait for specified delay
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    // Verify actual elapsed time using DWT — breakpoint if >27ms
    uint32_t dwt_now     = DWT->CYCCNT;
    uint32_t dwt_elapsed = dwt_now - dwt_last;
    dwt_last = dwt_now;
    // 30ms threshold at SystemCoreClock Hz
    if (dwt_elapsed > (SystemCoreClock / 1000u) * 30u) {
      PRINTF("WARN: audio_fb_task woken late by %lu ms!\n", (unsigned long)(dwt_elapsed / (SystemCoreClock / 1000u)));
    }

    // Guard against running the feedback loop when not playing.
    if (g_audio_state != AUDIO_STATE_PLAYING) {
        continue;               // Go back to sleep
    }

    // 1. Calculate Error
    int16_t error = tu_fifo_depth(spk_ff)/2 - (int16_t) g_audio_buf_lvl_filt_uint;
    
    // 2. PI Calculation
    proportional = KP * error;

    // 3. Limit adjustment
    if (proportional > MAX_RATE_ADJUSTMENT) proportional = MAX_RATE_ADJUSTMENT;
    else if (proportional < -MAX_RATE_ADJUSTMENT) proportional = -MAX_RATE_ADJUSTMENT;

    // 4. Calculate Feedback
    adjusted_rate = current_sample_rate_f + proportional;
    
    // RW612 is High Speed USB (Microframes, 125us) -> Divide by 8000.0f
    // If Full Speed is forced (1ms frames) -> Divide by 1000.0f
    samples_per_frame = adjusted_rate / 8000.0f;
    
    // 16.16 Fixed Point
    feedback_value = (uint32_t)(samples_per_frame * 65536.0f);

    audio_feedback_forward_metric(feedback_value);

    // Periodic feedback loop status (every ~1s = 40 iterations at 25ms)
    {
      static uint32_t fb_log_cnt = 0;
      if (++fb_log_cnt % 40 == 0) {
        int rate_int = (int)adjusted_rate;
        int rate_frac = abs((int)((adjusted_rate - rate_int) * 10));
        PRINTF("[FB] rate=%d.%d err=%d lvl=%u fb=0x%08lX\r\n",
               rate_int, rate_frac, (int)error,
               (unsigned)g_audio_buf_lvl_filt_uint,
               (unsigned long)feedback_value);
      }
    }

    #if AUDIO_DEBUG_LVL > 0
    static uint32_t count = 0;
    if (count % 1 == 0) {
      int adjusted_rate_int = (int)adjusted_rate; 
      // Added abs() here just for safety, though rate is always positive
      int adjusted_rate_frac = abs((int)((adjusted_rate - adjusted_rate_int) * 10)); 

      int proportional_int = (int)proportional; 
      int proportional_frac = abs((int)((proportional - proportional_int) * 10)); 
      
      // The magic string to fix the -0.x bug
      const char* p_sign = (proportional < 0.0f && proportional_int == 0) ? "-" : "";

      PRINTF("fs: %d.%d lvl: %u g_lvl_filt: %d E: %d, P: %s%d.%d\n", 
            adjusted_rate_int,
            adjusted_rate_frac,
            tu_fifo_count(spk_ff),
            (int)g_audio_buf_lvl_filt_uint,
            error,
            p_sign,             // Insert the minus sign if we are between 0 and -1
            proportional_int,
            proportional_frac);
    }
    count++;
    #endif
    
  }
}

/**
 * @brief Routes the calculated feedback value to either its own USB 
 * endpoint or alternatively to the dongle's USB endpoint via UDP.
 * @details Checks the global application mode and calls the 
 * appropriate feedback mechanism. For USB, it updates the endpoint.
 * For UDP, it queues a control packet.
 * * @param value_16_16 The calculated feedback value in 16.16 fixed-point format.
 * (e.g., relative to the sample rate).
 */
static void audio_feedback_forward_metric(uint32_t value_16_16)
{
    // Check the current application mode to decide where to send feedback
    app_mode_t current_mode = get_app_mode();

    switch (current_mode)
    {
        case MODE_USB_AUDIO:
            // Send directly via USB feedback endpoint
            tud_audio_fb_set(value_16_16);
            break;

        case MODE_UDP_HEADSET_AUDIO:
            udp_queue_feedback(value_16_16);
            if (g_udp_tx_task_handle != NULL)
                xTaskNotifyGive(g_udp_tx_task_handle);
            break;

        case MODE_RAW_HEADSET_AUDIO:
            raw_queue_feedback(value_16_16);
            if (g_raw_tx_task_handle != NULL)
                xTaskNotifyGive(g_raw_tx_task_handle);
            break;

        case MODE_UDP_DONGLE_AUDIO:
            configASSERT(false);
            break;

        default:
            break;
    }
}

void audio_reset_state(void)
{
    // Ensure audio task is stopped before touching shared resources.
    // Safe to call even if already suspended. Don't call from audio task, it would suspend itself.
    vTaskSuspend(g_audio_task_handle);

    // 1. Hard Stop DMA
    I2S_TransferAbortDMA(FLEXCOMM0_PERIPHERAL, &FLEXCOMM0_Tx_DMA_Handle);

    // 2. Clear Queue
    size_q_clear();

    // 3. Reset audio state
    g_audio_state = AUDIO_STATE_BUFFERING;
    g_audio_buf_lvl_filt_f = -1.0f;

    // 4. Nullify pointers
    spk_ff = NULL;
    mic_ff = NULL;
}


//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  g_blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  g_blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  g_blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  g_blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
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
    g_blink_interval_ms = BLINK_MOUNTED;
    // CRITICAL: Stop the audio task and ISR from touching the queue or DMA
    // while abort and clear operations are performed.
    taskENTER_CRITICAL();
    g_audio_state = AUDIO_STATE_BUFFERING;
    g_audio_buf_lvl_filt_f = -1.0f; // Reset filter to be re-initialized on next run
    I2S_TransferAbortDMA(FLEXCOMM0_PERIPHERAL, &FLEXCOMM0_Tx_DMA_Handle);
    size_q_clear();
    taskEXIT_CRITICAL();
  }

  return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

  TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
  if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0) {
    g_blink_interval_ms = BLINK_STREAMING;

    // Set initial feedback value at the nominal sample rate so the host
    // can start sending data immediately.  Linux snd-usb-audio waits for
    // a valid feedback packet before it begins isochronous transfers on
    // async OUT endpoints — without this the stream never starts.
    float samples_per_frame = (float)current_sample_rate / 8000.0f; // HS microframes
    uint32_t fb_nominal = (uint32_t)(samples_per_frame * 65536.0f); // 16.16 fixed-point
    tud_audio_fb_set(fb_nominal);
  }

  // Clear buffer when streaming format is changed
  if (alt != 0) {
    current_resolution = resolutions_per_format[alt - 1];
  }

  return true;
}


bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    // Only wake if we have a full packet.
    app_mode_t mode = get_app_mode();
    uint16_t available = tud_audio_available();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    switch (mode)
    {
      case MODE_USB_AUDIO:
        if (available >= MIN_DMA_BLOCK_SIZE)
        {
            // Wake the audio task!
            if (g_audio_task_handle != NULL) {
                vTaskNotifyGiveFromISR(g_audio_task_handle, &xHigherPriorityTaskWoken);
            }
        }
        break;

      case MODE_UDP_DONGLE_AUDIO:
        if (available >= UDP_PACKET_SIZE) {
            if (g_udp_tx_task_handle != NULL)
                vTaskNotifyGiveFromISR(g_udp_tx_task_handle, &xHigherPriorityTaskWoken);
        }
        break;

      case MODE_RAW_DONGLE_AUDIO:
        if (available >= RAW_AUDIO_PACKET_SIZE) {
            if (g_raw_tx_task_handle != NULL)
                vTaskNotifyGiveFromISR(g_raw_tx_task_handle, &xHigherPriorityTaskWoken);
        }
        break;

      default:
        break;
    }
    
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return true;
}