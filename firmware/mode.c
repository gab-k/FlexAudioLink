#include "mode.h"

static volatile app_mode_t app_mode = MODE_IDLE;

extern tusb_desc_device_t const desc_composite;
extern tusb_desc_device_t const desc_cdc_only;


/**
 * @brief Returns the string name of the current mode (helper for UI/CLI)
 */
const char* get_app_mode_name(app_mode_t mode) {
    switch(mode) {
        case MODE_IDLE: return "IDLE";
        case MODE_USB_AUDIO: return "USB AUDIO";
        case MODE_UDP_DONGLE_AUDIO: return "UDP DONGLE AUDIO";
        case MODE_UDP_HEADSET_AUDIO: return "UDP HEADSET AUDIO";
        case MODE_BLE_AUDIO: return "BLE AUDIO";
        default:             return "UNKNOWN MODE!";
    }
}

/**
 * @brief Transitions the system to a new operating mode.
 * @details Handles stopping previous tasks and starting new ones safely.
 * @param[in] target_mode The target mode to switch to.
 * @return true if transition successful (or already in mode), false if failed (e.g. missing tasks).
 */
bool set_current_app_mode(app_mode_t target_mode)
{
    if (app_mode == target_mode) return true;

    // This prevents HardFaults if xTaskCreate failed at boot.
    // Safety check for NULL task handles.
    if (g_wifi_task_handle == NULL) return false;
    if (g_udp_task_handle == NULL) return false;
    if (g_audio_task_handle == NULL) return false;
    if (g_audio_fb_task_handle == NULL) return false;

    // Needed for determining if USB re-enumeration is needed
    usb_desc_profile_t current_profile = get_usb_profile_for_mode(app_mode);
    usb_desc_profile_t target_profile  = get_usb_profile_for_mode(target_mode);

    // Suspend tasks
    vTaskSuspend(g_wifi_task_handle);
    vTaskSuspend(g_udp_task_handle);
    vTaskSuspend(g_audio_task_handle);
    vTaskSuspend(g_audio_fb_task_handle);

    // Reset audio state and abort I2S DMA.
    audio_reset_state();

    // Resume needed tasks
    switch (target_mode)
    {
        case MODE_USB_AUDIO:
            vTaskResume(g_audio_task_handle);
            vTaskResume(g_audio_fb_task_handle);
            break;

        case MODE_UDP_DONGLE_AUDIO:
            vTaskResume(g_wifi_task_handle);
            vTaskResume(g_udp_task_handle);
            // Notify Wi-Fi task to re-evaluate configuration (AP/STA)
            xTaskNotifyGive(g_wifi_task_handle);
            break;
        
        case MODE_UDP_HEADSET_AUDIO:
            vTaskResume(g_wifi_task_handle);
            vTaskResume(g_udp_task_handle);
            vTaskResume(g_audio_task_handle);
            // Notify Wi-Fi task to re-evaluate configuration (AP/STA)
            xTaskNotifyGive(g_wifi_task_handle);
            break;

        case MODE_BLE_AUDIO:
            // Placeholder, return false as its not implemented
            return false;

        case MODE_IDLE:
        default:
            break;
    }

    app_mode = target_mode;

    // Only switch if the profile is actually different
    if (current_profile != target_profile) 
    {
        // Disconnect for now to allow host to re-enumerate
        tud_disconnect();
        // Connect again to trigger re-enumeration
        // This needs to be done after app_mode is updated.
        // The descriptor callback returns the descriptors based on app_mode.
        tud_connect();
    }

    return true;
}

app_mode_t get_app_mode(void)
{
    return app_mode;
}

usb_desc_profile_t get_usb_profile_for_mode(app_mode_t mode)
{
    switch (mode) {
        case MODE_IDLE:
        case MODE_UDP_HEADSET_AUDIO:
            return USB_DESC_PROFILE_CDC_ONLY;
            
        case MODE_USB_AUDIO:
        case MODE_UDP_DONGLE_AUDIO:
        case MODE_BLE_AUDIO:
        default:
            return USB_DESC_PROFILE_COMPOSITE;
    }
}

// Designed to be called from audio task for retrieving active FIFO pointers.
// Arguments are tu_fifo_t ** (Pointer to a Pointer)
void get_active_fifos(tu_fifo_t **spk_ff_ptr, tu_fifo_t **mic_ff_ptr)
{
    switch (app_mode)
    {
        case MODE_USB_AUDIO:
            // Return the cached pointer (might be NULL if cable unplugged / not mounted)
            *spk_ff_ptr = tud_audio_get_ep_out_ff();
            *mic_ff_ptr = tud_audio_get_ep_in_ff();
            break;

        case MODE_UDP_HEADSET_AUDIO:
            // Headset receives audio from UDP -> Plays to I2S
            *spk_ff_ptr = udp_get_spk_fifo();
            *mic_ff_ptr = udp_get_mic_fifo();
            break;

        case MODE_BLE_AUDIO:
            // Placeholder, not implemented
            *spk_ff_ptr = NULL;
            *mic_ff_ptr = NULL;
            break;

        case MODE_UDP_DONGLE_AUDIO:
        case MODE_IDLE:
        default:
            // This function is expected to be called from audio task.
            // In these modes the audio task should not be active.
            configASSERT(false); // Not supported
            return;
    }
}