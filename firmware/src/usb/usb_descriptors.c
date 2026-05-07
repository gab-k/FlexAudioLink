#include <string.h>

#include <zephyr/drivers/hwinfo.h>

#include "app_control.h"
#include "usb/usb_descriptors.h"

#include "tusb.h"

//--------------------------------------------------------------------+
// PID MAPPING
//--------------------------------------------------------------------+
// USB and PFSK dongle enumerate as UAC+CDC. PFSK headset is CDC-only.
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_VID          0xCafe
#define USB_BCD          0x0200

#define USB_PID_UAC_CDC  (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | \
                          PID_MAP(HID, 2) | PID_MAP(MIDI, 3) | PID_MAP(AUDIO, 4) | \
                          PID_MAP(VENDOR, 5))
#define USB_PID_CDC_ONLY (0x4000 | PID_MAP(CDC, 0))

static bool usb_descriptors_cdc_only(void)
{
    return app_control_get_current_mode() == APP_MODE_PFSK_HEADSET;
}

//--------------------------------------------------------------------+
// UAC2 DESCRIPTOR TEMPLATES
//--------------------------------------------------------------------+

#define TUD_AUDIO20_HEADSET_STEREO_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN\
    + TUD_AUDIO20_DESC_STD_AC_LEN\
    + TUD_AUDIO20_DESC_CS_AC_LEN\
    + TUD_AUDIO20_DESC_CLK_SRC_LEN\
    + TUD_AUDIO20_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2)\
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN\
    + TUD_AUDIO20_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN\
    + TUD_AUDIO20_DESC_STD_AC_INT_EP_LEN\
    /* Interface 1, Alternate 0 (Speaker) */\
    + TUD_AUDIO20_DESC_STD_AS_LEN\
    /* Interface 1, Alternate 1 (Speaker) */\
    + TUD_AUDIO20_DESC_STD_AS_LEN\
    + TUD_AUDIO20_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN\
    + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN\
    /* Interface 2, Alternate 0 (Microphone) */\
    + TUD_AUDIO20_DESC_STD_AS_LEN\
    /* Interface 2, Alternate 1 (Microphone) */\
    + TUD_AUDIO20_DESC_STD_AS_LEN\
    + TUD_AUDIO20_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN\
    + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define TUD_AUDIO20_HEADSET_STEREO_DESCRIPTOR(_stridx, _epout, _epin, _epint) \
    /* Standard Interface Association Descriptor (IAD) */\
    TUD_AUDIO20_DESC_IAD(/*_firstitf*/ ITF_NUM_AUDIO_CONTROL, /*_nitfs*/ 3, /*_stridx*/ 0x00),\
    /* Standard AC Interface Descriptor(4.7.1) */\
    TUD_AUDIO20_DESC_STD_AC(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AC Interface Header Descriptor(4.7.2) */\
    TUD_AUDIO20_DESC_CS_AC(/*_bcdADC*/ 0x0200, /*_category*/ AUDIO20_FUNC_HEADSET, /*_totallen*/ TUD_AUDIO20_DESC_CLK_SRC_LEN+TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2)+TUD_AUDIO20_DESC_INPUT_TERM_LEN+TUD_AUDIO20_DESC_OUTPUT_TERM_LEN+TUD_AUDIO20_DESC_INPUT_TERM_LEN+TUD_AUDIO20_DESC_OUTPUT_TERM_LEN, /*_ctrl*/ AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS),\
    /* Clock Source Descriptor(4.7.2.1) */\
    TUD_AUDIO20_DESC_CLK_SRC(/*_clkid*/ UAC2_ENTITY_CLOCK, /*_attr*/ AUDIO20_CLOCK_SOURCE_ATT_INT_PRO_CLK, /*_ctrl*/ (AUDIO20_CTRL_RW << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), /*_assocTerm*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,  /*_stridx*/ 0x00),\
    /* Input Terminal Descriptor(4.7.2.4) - Speaker Path */\
    TUD_AUDIO20_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00, /*_clkid*/ UAC2_ENTITY_CLOCK, /*_nchannelslogical*/ 0x02, /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_ctrl*/ 0, /*_stridx*/ 0x00),\
    /* Feature Unit Descriptor(4.7.2.8) - Speaker Path */\
    TUD_AUDIO20_DESC_FEATURE_UNIT(/*_unitid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, /*_srcid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_stridx*/ 0x00, /*_ctrlch0master*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), /*_ctrlch1*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), /*_ctrlch2*/ (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)),\
    /* Output Terminal Descriptor(4.7.2.5) - Speaker Path */\
    TUD_AUDIO20_DESC_OUTPUT_TERM(/*_termid*/ UAC2_ENTITY_SPK_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES, /*_assocTerm*/ 0x00, /*_srcid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, /*_clkid*/ UAC2_ENTITY_CLOCK, /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),\
    /* Input Terminal Descriptor(4.7.2.4) - Mic Path */\
    TUD_AUDIO20_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC, /*_assocTerm*/ 0x00, /*_clkid*/ UAC2_ENTITY_CLOCK, /*_nchannelslogical*/ 0x01, /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_ctrl*/ 0, /*_stridx*/ 0x00),\
    /* Output Terminal Descriptor(4.7.2.5) - Mic Path */\
    TUD_AUDIO20_DESC_OUTPUT_TERM(/*_termid*/ UAC2_ENTITY_MIC_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00, /*_srcid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL, /*_clkid*/ UAC2_ENTITY_CLOCK, /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),\
    /* Standard AC Interrupt Endpoint Descriptor(4.8.2.1) */\
    TUD_AUDIO20_DESC_STD_AC_INT_EP(/*_ep*/ _epint, /*_interval*/ 16), \
    /******************************************************************/\
    /*                  Speaker Interface Descriptors                 */\
    /******************************************************************/\
    /* Standard AS Interface Descriptor(4.9.1) */\
    /* Interface 1, Alternate 0 - default alternate setting with 0 bandwidth */\
    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING_SPK, /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.9.1) */\
    /* Interface 1, Alternate 1 - alternate interface for data streaming */\
    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING_SPK, /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.9.2) */\
    TUD_AUDIO20_DESC_CS_AS_INT(/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_ctrl*/ AUDIO20_CTRL_NONE, /*_formattype*/ AUDIO20_FORMAT_TYPE_I, /*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM, /*_nchannelsphysical*/ CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX, /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, /*_stridx*/ 0x00),\
    /* Type I Format Type Descriptor(2.3.1.6 - Audio Formats) */\
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */\
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(/*_ep*/ _epout, /*_attr*/ (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA), /*_maxEPsize*/ TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX), /*_interval*/ 0x01),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */\
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, /*_ctrl*/ AUDIO20_CTRL_NONE, /*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001),\
    /******************************************************************/\
    /*                Microphone Interface Descriptors                */\
    /******************************************************************/\
    /* Standard AS Interface Descriptor(4.9.1) */\
    /* Interface 2, Alternate 0 - default alternate setting with 0 bandwidth */\
    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING_MIC, /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.9.1) */\
    /* Interface 2, Alternate 1 - alternate interface for data streaming */\
    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING_MIC, /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.9.2) */\
    TUD_AUDIO20_DESC_CS_AS_INT(/*_termid*/ UAC2_ENTITY_MIC_OUTPUT_TERMINAL, /*_ctrl*/ AUDIO20_CTRL_NONE, /*_formattype*/ AUDIO20_FORMAT_TYPE_I, /*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM, /*_nchannelsphysical*/ CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX, /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, /*_stridx*/ 0x00),\
    /* Type I Format Type Descriptor(2.3.1.6 - Audio Formats) */\
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.10.1.1) */\
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(/*_ep*/ _epin, /*_attr*/ (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), /*_maxEPsize*/ TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX), /*_interval*/ 0x01),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */\
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, /*_ctrl*/ AUDIO20_CTRL_NONE, /*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, /*_lockdelay*/ 0x0000)

//--------------------------------------------------------------------+
// UAC1 DESCRIPTOR TEMPLATES
//--------------------------------------------------------------------+

#define TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(_nfreqs) (\
    + TUD_AUDIO10_DESC_STD_AC_LEN\
    + TUD_AUDIO10_DESC_CS_AC_LEN(2)\
    + TUD_AUDIO10_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)\
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN\
    /* Interface 1, Alternate 0 (speaker) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    /* Interface 1, Alternate 1 (speaker) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs)\
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN\
    /* Interface 2, Alternate 0 (microphone) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    /* Interface 2, Alternate 1 (microphone) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs)\
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(_itfnum, _stridx, _nBytesPerSample_RX, _nBitsUsedPerSample_RX, _nBytesPerSample_TX, _nBitsUsedPerSample_TX, _epout, _epoutsize, _epin, _epinsize, ...) \
    /* Standard AC Interface Descriptor(4.3.1) */\
    TUD_AUDIO10_DESC_STD_AC(/*_itfnum*/ _itfnum, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Class-Specific AC Interface Header Descriptor(4.3.2) */\
    TUD_AUDIO10_DESC_CS_AC(/*_bcdADC*/ 0x0100, /*_totallen*/ (TUD_AUDIO10_DESC_INPUT_TERM_LEN+TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)+TUD_AUDIO10_DESC_OUTPUT_TERM_LEN+TUD_AUDIO10_DESC_INPUT_TERM_LEN+TUD_AUDIO10_DESC_OUTPUT_TERM_LEN), /*_itf*/ ((_itfnum)+1), ((_itfnum)+2)),\
    /* Speaker Input Terminal Descriptor(4.3.2.1) */\
    TUD_AUDIO10_DESC_INPUT_TERM(/*_termid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_nchannels*/ 0x02, /*_channelcfg*/ AUDIO10_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_stridx*/ 0x00),\
    /* Speaker Feature Unit Descriptor(4.3.2.5) */\
    TUD_AUDIO10_DESC_FEATURE_UNIT(/*_unitid*/ UAC1_ENTITY_SPK_FEATURE_UNIT, /*_srcid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_stridx*/ 0x00, /*_ctrlmaster*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), /*_ctrlch1*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), /*_ctrlch2*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME)),\
    /* Speaker Output Terminal Descriptor(4.3.2.2) */\
    TUD_AUDIO10_DESC_OUTPUT_TERM(/*_termid*/ UAC1_ENTITY_SPK_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES, /*_assocTerm*/ UAC1_ENTITY_MIC_INPUT_TERMINAL, /*_srcid*/ UAC1_ENTITY_SPK_FEATURE_UNIT, /*_stridx*/ 0x00),\
    /* Microphone Input Terminal Descriptor(4.3.2.1) */\
    TUD_AUDIO10_DESC_INPUT_TERM(/*_termid*/ UAC1_ENTITY_MIC_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC, /*_assocTerm*/ UAC1_ENTITY_SPK_OUTPUT_TERMINAL, /*_nchannels*/ 0x01, /*_channelcfg*/ AUDIO10_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_stridx*/ 0x00),\
    /* Microphone Output Terminal Descriptor(4.3.2.2) */\
    TUD_AUDIO10_DESC_OUTPUT_TERM(/*_termid*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_srcid*/ UAC1_ENTITY_MIC_INPUT_TERMINAL, /*_stridx*/ 0x00),\
    /* Standard AS Interface Descriptor(4.5.1) - Speaker Interface 1, Alternate 0 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+1), /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.5.1) - Speaker Interface 1, Alternate 1 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+1), /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.5.2) */\
    TUD_AUDIO10_DESC_CS_AS_INT(/*_termid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_delay*/ 0x01, /*_formattype*/ AUDIO10_DATA_FORMAT_TYPE_I_PCM),\
    /* Type I Format Type Descriptor(2.2.5) */\
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(/*_nrchannels*/ 0x02, /*_subframesize*/ _nBytesPerSample_RX, /*_bitresolution*/ _nBitsUsedPerSample_RX, /*_freqs*/ __VA_ARGS__),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.6.1.1) */\
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(/*_ep*/ _epout, /*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE), /*_maxEPsize*/ _epoutsize, /*_interval*/ 0x01, /*_syncep*/ 0x00),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.6.1.2) */\
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, /*_lockdelayunits*/ AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001),\
    /* Standard AS Interface Descriptor(4.5.1) - Microphone Interface 2, Alternate 0 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+2), /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.5.1) - Microphone Interface 2, Alternate 1 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+2), /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.5.2) */\
    TUD_AUDIO10_DESC_CS_AS_INT(/*_termid*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_delay*/ 0x01, /*_formattype*/ AUDIO10_DATA_FORMAT_TYPE_I_PCM),\
    /* Type I Format Type Descriptor(2.2.5) */\
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(/*_nrchannels*/ 0x01, /*_subframesize*/ _nBytesPerSample_TX, /*_bitresolution*/ _nBitsUsedPerSample_TX, /*_freqs*/ __VA_ARGS__),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.6.1.1) */\
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(/*_ep*/ _epin, /*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS), /*_maxEPsize*/ _epinsize, /*_interval*/ 0x01, /*_syncep*/ 0x00),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.6.1.2) */\
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, /*_lockdelayunits*/ AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001)

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device_uac_cdc =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID_UAC_CDC,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

static tusb_desc_device_t const desc_device_cdc =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID_CDC_ONLY,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)(usb_descriptors_cdc_only() ? &desc_device_cdc : &desc_device_uac_cdc);
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
// Audio Endpoints
#define EPNUM_AUDIO_SPK_OUT     0x01
#define EPNUM_AUDIO_MIC_IN      0x81
#define EPNUM_AUDIO_INT_IN      0x82

// CDC Endpoints
#define EPNUM_CDC_NOTIF         0x83
#define EPNUM_CDC_OUT           0x02
#define EPNUM_CDC_IN            0x84

enum {
    ITF_NUM_CDC_ONLY = 0,
    ITF_NUM_CDC_ONLY_DATA,
    ITF_NUM_CDC_ONLY_TOTAL,
};

// ==========================================
// CONFIG A: UAC + CDC
// ==========================================
#if TUD_OPT_HIGH_SPEED
#define CONFIG_UAC2_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO20_HEADSET_STEREO_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)
uint8_t const desc_hs_configuration_uac_cdc[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_UAC2_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_AUDIO20_HEADSET_STEREO_DESCRIPTOR(/*_stridx*/ STRID_UAC2, EPNUM_AUDIO_SPK_OUT, EPNUM_AUDIO_MIC_IN, EPNUM_AUDIO_INT_IN),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_hs_configuration_uac_cdc) == CONFIG_UAC2_TOTAL_LEN, "Incorrect UAC2 size");
#endif

#define CONFIG_UAC1_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(1) + CFG_TUD_CDC * TUD_CDC_DESC_LEN)
#define UAC1_EP_OUT_SIZE TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define UAC1_EP_IN_SIZE  TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
uint8_t const desc_fs_configuration_uac_cdc[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_UAC1_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(
        /*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_stridx*/ STRID_UAC1,
        /* RX */ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
        /* TX */ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX,
        /* EP */ EPNUM_AUDIO_SPK_OUT, UAC1_EP_OUT_SIZE,
        /* EP */ EPNUM_AUDIO_MIC_IN,  UAC1_EP_IN_SIZE,
        /* Freqs */ CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
    ),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_fs_configuration_uac_cdc) == CONFIG_UAC1_TOTAL_LEN, "Incorrect UAC1 size");

// ==========================================
// CONFIG B: CDC only
// ==========================================
#define CONFIG_CDC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

#if TUD_OPT_HIGH_SPEED
uint8_t const desc_hs_configuration_cdc[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_CDC_ONLY_TOTAL, 0, CONFIG_CDC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_ONLY, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_hs_configuration_cdc) == CONFIG_CDC_TOTAL_LEN, "Incorrect HS CDC size");
#endif

uint8_t const desc_fs_configuration_cdc[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_CDC_ONLY_TOTAL, 0, CONFIG_CDC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_ONLY, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};
TU_VERIFY_STATIC(sizeof(desc_fs_configuration_cdc) == CONFIG_CDC_TOTAL_LEN, "Incorrect FS CDC size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;

#if TUD_OPT_HIGH_SPEED
    if (usb_descriptors_cdc_only()) {
        return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_hs_configuration_cdc : desc_fs_configuration_cdc;
    }

    return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_hs_configuration_uac_cdc : desc_fs_configuration_uac_cdc;
#else
    return usb_descriptors_cdc_only() ? desc_fs_configuration_cdc : desc_fs_configuration_uac_cdc;
#endif
}

#if TUD_OPT_HIGH_SPEED
tusb_desc_device_qualifier_t const desc_device_qualifier =
{
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = USB_BCD,
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

    if (usb_descriptors_cdc_only()) {
        return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_fs_configuration_cdc : desc_hs_configuration_cdc;
    }

    return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_fs_configuration_uac_cdc : desc_hs_configuration_uac_cdc;
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

static uint16_t desc_str[32 + 1];

static const char *usb_product_string(void)
{
    switch (app_control_get_current_mode()) {
    case APP_MODE_USB:
        return "FlexAudioLink (Direct USB Audio)";
    case APP_MODE_PFSK_DONGLE:
        return "FlexAudioLink (Dongle)";
    case APP_MODE_PFSK_HEADSET:
        return "FlexAudioLink (Headset)";
    default:
        return "FlexAudioLink";
    }
}

static size_t usb_get_serial_string(uint16_t *utf16_out, size_t max_chars)
{
	uint8_t device_id[16];
	ssize_t id_len;
	size_t chr_count;
	static const char hex[] = "0123456789ABCDEF";

	id_len = hwinfo_get_device_id(device_id, sizeof(device_id));
	if (id_len <= 0) {
		const char *fallback = "ERROR: Can't Retrieve!";

		chr_count = strlen(fallback);
		if (chr_count > max_chars) {
			chr_count = max_chars;
		}

		for (size_t i = 0; i < chr_count; i++) {
			utf16_out[i] = fallback[i];
		}

		return chr_count;
	}

	chr_count = (size_t)id_len * 2U;
	if (chr_count > max_chars) {
		chr_count = max_chars;
	}

	for (size_t i = 0; i < chr_count / 2U; i++) {
		utf16_out[i * 2U + 0U] = hex[(device_id[i] >> 4) & 0x0F];
		utf16_out[i * 2U + 1U] = hex[device_id[i] & 0x0F];
	}

	return chr_count;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    const char *str;
    size_t chr_count;

    (void) langid;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * 1 + 2));
        return desc_str;
    }

    if (index == STRID_SERIAL) {
        chr_count = usb_get_serial_string(desc_str + 1, 32);
        desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
        return desc_str;
    }

    if (index == STRID_PRODUCT) {
        str = usb_product_string();
    } else {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        str = string_desc_arr[index];
    }

    chr_count = strlen(str);
    if (chr_count > 32) {
        chr_count = 32;
    }

    for (size_t i = 0; i < chr_count; i++) {
        desc_str[1 + i] = str[i];
    }

    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
