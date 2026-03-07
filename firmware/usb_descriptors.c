#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "mode.h"
#include "log.h"
#include "util.h"

//--------------------------------------------------------------------+
// PID MAPPING 
//--------------------------------------------------------------------+
// Defining two different PIDs. This is CRITICAL especially for Windows.
// Using the same PID for two different descriptor layouts 
// (one with Audio, one without), might lead to issues in the OS driver.
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
// PID for Composite Mode (CDC + Audio)
#define USB_PID_COMPOSITE (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | PID_MAP(HID, 2) | \
                           PID_MAP(MIDI, 3) | PID_MAP(AUDIO, 4) | PID_MAP(VENDOR, 5) )
// PID for CDC Only Mode (CLI Mode) - Just CDC bit set
#define USB_PID_CDC_ONLY  (0x4000 | PID_MAP(CDC, 0))

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
// 1. COMPOSITE DEVICE DESCRIPTOR (CDC + AUDIO)
tusb_desc_device_t const desc_composite =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,
    .idProduct          = USB_PID_COMPOSITE,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};
// 2. CDC ONLY DEVICE DESCRIPTOR
tusb_desc_device_t const desc_cdc_only =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Use IAD for CDC compatibility
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,
    .idProduct          = USB_PID_CDC_ONLY,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x06, // "TinyUSB CDC"
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

//--------------------------------------------------------------------+
// Device Descriptor Callback
//--------------------------------------------------------------------+


uint8_t const * tud_descriptor_device_cb(void)
{
  // Get the GLOBAL app mode
  app_mode_t current_mode = get_app_mode();

  switch (current_mode)
  {
      case MODE_IDLE:
      case MODE_UDP_HEADSET_AUDIO:
      case MODE_BLE_AUDIO:
          return (uint8_t const *) &desc_cdc_only;
      
      case MODE_USB_AUDIO:
      case MODE_UDP_DONGLE_AUDIO:
      default:
          return (uint8_t const *) &desc_composite;
  }
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
// Audio Endpoints
#define EPNUM_AUDIO_SPK_OUT     0x01
#define EPNUM_AUDIO_MIC_IN      0x81
#define EPNUM_AUDIO_INT_IN      0x82
#define EPNUM_AUDIO_SPK_FB_IN   0x83

// CDC Endpoints
#define EPNUM_CDC_NOTIF         0x84
#define EPNUM_CDC_OUT           0x02
#define EPNUM_CDC_IN            0x85


// ==========================================
// CONFIG A: COMPOSITE (Audio + CDC)
// ==========================================
// High-Speed uses UAC2
#if TUD_OPT_HIGH_SPEED
#define CONFIG_UAC2_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO20_HEADSET_STEREO_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)
uint8_t const desc_uac2_configuration[] =
{
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_UAC2_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    
    // Pass all endpoint numbers to your corrected descriptor macro
    TUD_AUDIO20_HEADSET_STEREO_DESCRIPTOR(/*_stridx*/ 5, EPNUM_AUDIO_SPK_OUT, EPNUM_AUDIO_MIC_IN, EPNUM_AUDIO_INT_IN, EPNUM_AUDIO_SPK_FB_IN),

    // Interface number, String index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_uac2_configuration) == CONFIG_UAC2_TOTAL_LEN, "Incorrect UAC2 size");
#endif
// Full-Speed uses UAC1
#define CONFIG_UAC1_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(1) + CFG_TUD_CDC * TUD_CDC_DESC_LEN)
#define UAC1_EP_OUT_SIZE TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define UAC1_EP_IN_SIZE  TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
uint8_t const desc_uac1_configuration[] =
{
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_UAC1_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(
        /*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_stridx*/ 4,
        /* RX */ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
        /* TX */ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX,
        /* EP */ EPNUM_AUDIO_SPK_OUT, UAC1_EP_OUT_SIZE,
        /* EP */ EPNUM_AUDIO_MIC_IN,  UAC1_EP_IN_SIZE,
        /* Freqs */ 48000
    ),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_uac1_configuration) == CONFIG_UAC1_TOTAL_LEN, "Incorrect UAC1 size");


// ==========================================
// CONFIG B: CDC ONLY (New Implementation)
// ==========================================
#define CONFIG_CDC_ONLY_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
uint8_t const desc_configuration_cdc_only[] =
{
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_CDC_ONLY, 0, CONFIG_CDC_ONLY_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    // Note: Reuse the SAME EPNUMs as the composite mode. This is safe and easier to manage.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_ONLY, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_configuration_cdc_only) == CONFIG_CDC_ONLY_TOTAL_LEN,
                 "Incorrect CDC-Only config size");

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;

  // Get the GLOBAL app mode
  app_mode_t current_mode = get_app_mode();

  // 2. Return CDC Only descriptor if we are in a Wireless Mode
  if (current_mode == MODE_UDP_HEADSET_AUDIO || current_mode == MODE_BLE_AUDIO || current_mode == MODE_IDLE) {
    return desc_configuration_cdc_only;
  }

#if TUD_OPT_HIGH_SPEED
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_uac2_configuration : desc_uac1_configuration;
#else
  return desc_uac1_configuration;
#endif
}

#if TUD_OPT_HIGH_SPEED
tusb_desc_device_qualifier_t const desc_device_qualifier =
{
  .bLength            = sizeof(tusb_desc_device_qualifier_t),
  .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .bNumConfigurations = 0x01,
  .bReserved          = 0x00
};

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
  return (uint8_t const *) &desc_device_qualifier;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
  (void) index;
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_uac1_configuration : desc_uac2_configuration;
}
#endif

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+


static char const *string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 },  // 0: English
  "TinyUSB",                      // 1: Manufacturer
  "TinyUSB Headset",              // 2: Product
  "ERROR: Can't Retrieve!",       // 3: Serial
  "UAC1 Headset",                 // 4: UAC1 Interface
  "UAC2 Async Headset",           // 5: UAC2 Interface
  "TinyUSB CDC",                  // 6: CDC Interface
};

static uint16_t _desc_str[34 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  const char* str;
  char uid_str[35]; // Buffer for UID string
  size_t chr_count;
  size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;

  if ( index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * 1 + 2));
    return _desc_str;
  }
  
  if (index == 3) {
    uint8_t uid[16]; // Buffer for raw UID bytes
    uint32_t uid_len = sizeof(uid);
    get_unique_id(uid, &uid_len);
    // Convert the raw UID to a hex string (ASCII)
    snprintf(uid_str, sizeof(uid_str), "0x");
    for (size_t i = 0; i < uid_len; i++) {
        snprintf(uid_str + strlen(uid_str), sizeof(uid_str) - strlen(uid_str), "%02X", uid[i]);
    }
    // Point to the UID string
    str = uid_str;
  } else {
    if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;
    str = string_desc_arr[index];
  }

  chr_count = strlen(str);
  if ( chr_count > max_count ) chr_count = max_count;
  
  // Convert ASCII string into UTF-16
  for (size_t i = 0; i < chr_count; i++) {
    _desc_str[1 + i] = str[i];
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}