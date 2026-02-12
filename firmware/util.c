#include "util.h"

// LED blinking task changes LED color depending on mode
void blink_task(void *pvParameters) 
{
    TickType_t delay_ticks;
    while(1)
    {
        // Toggle First
        GPIO_PortToggle(GPIO, BOARD_INITLEDPINS_LED_GREEN_PORT, BOARD_INITLEDPINS_LED_GREEN_PIN_MASK);

        // Delay for g_blink_interval_ms milliseconds
        delay_ticks = pdMS_TO_TICKS(g_blink_interval_ms);
        configASSERT(delay_ticks > 0);
        vTaskDelay(delay_ticks);
    }
}


// Used for RTOS runtime stats
static uint32_t last_cycle_count = 0;
static uint64_t high_res_full_count = 0;
void vConfigureTimerForRunTimeStats(void) {
    // Reset software counters
    last_cycle_count = 0;
    high_res_full_count = 0;

    // Hardware Init 
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; 
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
// Used for RTOS runtime stats
uint64_t vGetRunTimeCounterValue64(void) {
    uint32_t current_cycle_count;
    uint64_t total_count;
    UBaseType_t uxSavedInterruptStatus;

    // Use ISR-safe critical section because this is called from vTaskSwitchContext
    uxSavedInterruptStatus = portSET_INTERRUPT_MASK_FROM_ISR();
    {
        current_cycle_count = DWT->CYCCNT;
        
        // Detect 32-bit overflow
        if (current_cycle_count < last_cycle_count) {
            high_res_full_count += 0x100000000ULL;
        }
        last_cycle_count = current_cycle_count;
        total_count = high_res_full_count + current_cycle_count;
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSavedInterruptStatus);
    
    return total_count;
}