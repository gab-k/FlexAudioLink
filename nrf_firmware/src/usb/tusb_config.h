#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_NRF54
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_ZEPHYR
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUH_ENABLED       0

#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             1
#define CFG_TUD_VENDOR            0

#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP                            1
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS                               1
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE                         48000
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX                           1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX                           2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX          2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX                  16
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX          2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX                  16

#define CFG_TUD_AUDIO_ENABLE_EP_IN                                   1
#define CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN                     \
	TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, \
			  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, \
			  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_IN                     \
	TUD_AUDIO_EP_SIZE(true, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,  \
			  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, \
			  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX                            \
	TU_MAX(CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN,              \
	       CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_IN)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ                         \
	TU_MAX(4 * CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN,          \
	       32 * CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_IN)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                                  1
#define CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT                    \
	TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, \
			  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, \
			  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_OUT                    \
	TUD_AUDIO_EP_SIZE(true, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,  \
			  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, \
			  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX                           \
	TU_MAX(CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT,             \
	       CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_OUT)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ                        \
	TU_MAX(4 * CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT,         \
	       32 * CFG_TUD_AUDIO20_FUNC_1_FORMAT_1_EP_SZ_OUT)

#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                             1

#define CFG_TUD_CDC_NOTIFY        1
#define CFG_TUD_CDC_RX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_EP_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_RX_EPSIZE     (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_EPSIZE     (TUD_OPT_HIGH_SPEED ? 512 : 64)

#ifdef __cplusplus
}
#endif
