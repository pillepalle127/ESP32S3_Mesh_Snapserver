/**
 * @file tusb_config.h
 * @brief TinyUSB configuration: a USB Audio Class 1 speaker with feedback
 *        endpoint, see usb_audio.c.
 *
 * Added to the TinyUSB library's include path by main/CMakeLists.txt. The
 * DWC2 settings follow espressif/esp_tinyusb's own tusb_config.h for the S3
 * (full speed, DMA); that component is replaced by an empty stand-in, see
 * components/esp_tinyusb.
 */
#pragma once

#include "esp_attr.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG            0
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED           1

/* DWC2: slave mode compiled in, DMA used where the hardware has it. DMA
 * reads and writes these buffers, so they stay in internal RAM. */
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_DWC2_DMA_ENABLE   1
#define CFG_TUSB_MEM_SECTION      TU_ATTR_ALIGNED(4) DRAM_ATTR
#define CFG_TUSB_MEM_ALIGN        TU_ATTR_ALIGNED(4)

#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

/* 48 kHz, 2 channels, 16 bit -- the one format offered, see usb_audio.c. */
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX         2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX 2
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX         16
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE       48000
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, \
                      CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)

/*
 * Receive FIFO: 48 ms. The capture loop empties it 20 ms at a time, and the
 * feedback (FIFO count method) holds the level at half, so it swings about
 * 14..34 ms. TinyUSB places it statically in internal RAM (~9.4 kB), on
 * every device whether USB audio is on or not.
 */
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (48 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT       1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP  1

#ifdef __cplusplus
}
#endif
