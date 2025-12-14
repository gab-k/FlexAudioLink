#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug. */
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID           (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | PID_MAP(HID, 2) | \
    PID_MAP(MIDI, 3) | PID_MAP(AUDIO, 4) | PID_MAP(VENDOR, 5) )

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define EPNUM_AUDIO_SPK_OUT     0x01
#define EPNUM_AUDIO_MIC_IN      0x81
#define EPNUM_AUDIO_INT_IN      0x82
#define EPNUM_AUDIO_SPK_FB_IN   0x83

// -- ADDED CDC ENDPOINTS --
// Ensure these do not conflict with Audio EPs
#define EPNUM_CDC_NOTIF         0x84
#define EPNUM_CDC_OUT           0x02
#define EPNUM_CDC_IN            0x85

// ** FIX: Corrected macro name and usage **
#define UAC1_EP_OUT_SIZE TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define UAC1_EP_IN_SIZE  TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)

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

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
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
  "1234-ABCD-FB",                 // 3: Serial
  "UAC1 Headset",                 // 4: UAC1 Interface
  "UAC2 Async Headset",           // 5: UAC2 Interface
  "TinyUSB CDC",                  // 6: CDC Interface
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  if ( index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];
    chr_count = strlen(str);
    size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
    if ( chr_count > max_count ) chr_count = max_count;

    for (size_t i = 0; i < chr_count; i++) {
      _desc_str[1 + i] = str[i];
    }
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}