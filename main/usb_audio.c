/**
 * @file usb_audio.c
 * @brief USB sound card on the server's native USB port, see usb_audio.h.
 *
 * USB Audio Class 1, not 2: the S3 is a full-speed device, and on full
 * speed every common host (Windows since 7, Linux, macOS) brings a UAC1
 * driver and reads the feedback endpoint in the format the spec gives
 * (10.14). TinyUSB's own uac2_speaker_fb example does the same and only
 * offers UAC2 on high speed. The descriptor template and the request
 * handling below follow that example (MIT, Copyright (c) 2023 HiFiPhile).
 */
#include "usb_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_cntl_struct.h"
#include "tusb.h"

static const char *TAG = "USB_AUDIO";

#define USB_AUDIO_RATE        48000U
#define USB_AUDIO_FRAME_BYTES (CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX)
/* Largest frame the capture loop asks for: 20 ms at 48 kHz. */
#define USB_AUDIO_MAX_FRAMES  960U

#define FIFO_BYTES        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ
#define BYTES_PER_MS      (USB_AUDIO_RATE / 1000U * USB_AUDIO_FRAME_BYTES)
/*
 * The feedback holds the FIFO's *average* level at half. The capture loop
 * takes a whole frame (20 ms) at once, so right before each read the level
 * sits half a frame above that: the nominal level computed in
 * usb_audio_fill_stereo(). Prebuffering fills to it, and the emergency
 * correction for a host that ignores the feedback skips or doubles one
 * frame per read once the level is more than SLIP_MARGIN off it. With the
 * feedback working, that never happens.
 */
#define SLIP_MARGIN_BYTES (8U * BYTES_PER_MS)

/*
 * "The PC is playing": the stream is open and a sample above this peak
 * (about -70 dBFS) came in within USB_AUDIO_RELEASE_US. Windows keeps the
 * stream open and sends zeros while nothing plays; without this the I2S
 * input would never get its turn back.
 */
#define SIGNAL_PEAK            10
#define USB_AUDIO_RELEASE_US   (2LL * 1000LL * 1000LL)

/* Volume range offered to the host, 1/256 dB. No gain above unity. */
#define VOLUME_MIN    (-60 * 256)
#define VOLUME_MAX    0
#define VOLUME_RES    256

#define USB_TASK_STACK    4096
#define USB_TASK_PRIORITY 3

/*
 * UAC1 stereo speaker with an asynchronous data endpoint and a feedback
 * (synch) endpoint, taken from uac2_speaker_fb/src/usb_descriptors.h. One
 * sample rate, 48 kHz. bRefresh 0 on the synch endpoint, as there: Windows
 * and macOS schedule the feedback by bRefresh, Linux by bInterval, and the
 * DWC2 driver needs both to mean every frame.
 */
#define UAC1_ENTITY_INPUT_TERMINAL  0x01
#define UAC1_ENTITY_FEATURE_UNIT    0x02
#define UAC1_ENTITY_OUTPUT_TERMINAL 0x03

#define USB_AUDIO_SPEAKER_DESC_LEN(_nfreqs) (\
  + TUD_AUDIO10_DESC_STD_AC_LEN\
  + TUD_AUDIO10_DESC_CS_AC_LEN(1)\
  + TUD_AUDIO10_DESC_INPUT_TERM_LEN\
  + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN\
  + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)\
  + TUD_AUDIO10_DESC_STD_AS_LEN\
  + TUD_AUDIO10_DESC_STD_AS_LEN\
  + TUD_AUDIO10_DESC_CS_AS_INT_LEN\
  + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs)\
  + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN\
  + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN\
  + TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP_LEN)

#define USB_AUDIO_SPEAKER_DESCRIPTOR(_itfnum, _stridx, _nBytesPerSample, _nBitsUsedPerSample, _epout, _epoutsize, _epfb, ...) \
  TUD_AUDIO10_DESC_STD_AC(_itfnum, 0x00, _stridx),\
  TUD_AUDIO10_DESC_CS_AC(0x0100, (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)), ((_itfnum) + 1)),\
  TUD_AUDIO10_DESC_INPUT_TERM(UAC1_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x02, AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0x00, 0x00),\
  TUD_AUDIO10_DESC_OUTPUT_TERM(UAC1_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x00, UAC1_ENTITY_FEATURE_UNIT, 0x00),\
  TUD_AUDIO10_DESC_FEATURE_UNIT(UAC1_ENTITY_FEATURE_UNIT, UAC1_ENTITY_INPUT_TERMINAL, 0x00, (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME)),\
  TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x00, 0x00, 0x00),\
  TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x01, 0x02, 0x00),\
  TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_INPUT_TERMINAL, 0x00, AUDIO10_DATA_FORMAT_TYPE_I_PCM),\
  TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x02, _nBytesPerSample, _nBitsUsedPerSample, __VA_ARGS__),\
  TUD_AUDIO10_DESC_STD_AS_ISO_EP(_epout, (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS), _epoutsize, 0x01, _epfb),\
  TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),\
  TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(_epfb, 0)

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_TOTAL
};

#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_FB  0x81

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_COUNT
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + USB_AUDIO_SPEAKER_DESC_LEN(1))

static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,      /* Espressif */
    .idProduct = 0x8000,     /* Espressif's test PID */
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_desc_configuration[] = {
    /* Bus powered, 500 mA: the board often runs off this cable. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    USB_AUDIO_SPEAKER_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_INTERFACE,
                                 CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
                                 CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX,
                                 EPNUM_AUDIO_OUT, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX,
                                 EPNUM_AUDIO_FB, USB_AUDIO_RATE),
};

TU_VERIFY_STATIC(sizeof(s_desc_configuration) == CONFIG_TOTAL_LEN, "Incorrect size");

/* "SnapMesh B688" and the MAC as serial, filled in by usb_audio_start(). */
static char s_product[24];
static char s_serial[16];
static uint16_t s_desc_str[32 + 1];

static bool s_started;
static usb_phy_handle_t s_phy;
static int16_t *s_scratch;

static volatile bool s_mounted;
static volatile bool s_streaming;
static bool s_prebuffering = true;
static bool s_active;
static int64_t s_last_signal_us;

/* Host volume/mute per channel, 0 = master; 1/256 dB. Set by control
 * requests (TinyUSB task), turned into gains for the capture loop. */
static int16_t s_volume[3];
static uint8_t s_mute[3];
static volatile float s_gain[2] = { 1.0f, 1.0f };

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_underrun_frames;
static uint32_t s_dropped_frames;
static uint32_t s_repeated_frames;

static void update_gains(void)
{
    for (int ch = 0; ch < 2; ++ch) {
        if (s_mute[0] != 0U || s_mute[ch + 1] != 0U) {
            s_gain[ch] = 0.0f;
            continue;
        }
        const float db = ((float)s_volume[0] + (float)s_volume[ch + 1]) / 256.0f;
        s_gain[ch] = (db >= 0.0f) ? 1.0f : powf(10.0f, db / 20.0f);
    }
}

/* ---- TinyUSB descriptor callbacks ---- */

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    const char *str = NULL;
    size_t count = 0;

    switch (index) {
    case STRID_LANGID:
        s_desc_str[1] = 0x0409; /* English */
        count = 1;
        break;
    case STRID_MANUFACTURER:
        str = "SnapMesh";
        break;
    case STRID_PRODUCT:
    case STRID_INTERFACE:
        str = s_product;
        break;
    case STRID_SERIAL:
        str = s_serial;
        break;
    default:
        return NULL; /* incl. 0xEE, the Microsoft OS descriptor */
    }

    if (str != NULL) {
        count = strlen(str);
        const size_t max_count = sizeof(s_desc_str) / sizeof(s_desc_str[0]) - 1U;
        if (count > max_count) {
            count = max_count;
        }
        for (size_t i = 0; i < count; ++i) {
            s_desc_str[1U + i] = (uint16_t)str[i];
        }
    }
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * count + 2U));
    return s_desc_str;
}

/* ---- Device state ---- */

void tud_mount_cb(void)
{
    s_mounted = true;
}

void tud_umount_cb(void)
{
    s_mounted = false;
    s_streaming = false;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_streaming = false;
}

/* ---- Audio class callbacks (UAC1) ---- */

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    if (tu_u16_low(p_request->wIndex) == ITF_NUM_AUDIO_STREAMING &&
        tu_u16_low(p_request->wValue) != 0U) {
        s_streaming = true;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    if (tu_u16_low(p_request->wIndex) == ITF_NUM_AUDIO_STREAMING &&
        tu_u16_low(p_request->wValue) == 0U) {
        s_streaming = false;
    }
    return true;
}

/* Sampling frequency, an endpoint control in UAC1. Only 48 kHz is offered. */
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;
    if (TU_U16_HIGH(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 3U) {
        const uint32_t rate = tu_unaligned_read32(buf) & 0x00FFFFFFU;
        if (rate != USB_AUDIO_RATE) {
            ESP_LOGW(TAG, "Host asked for %lu Hz; only %u Hz is offered", (unsigned long)rate,
                     (unsigned)USB_AUDIO_RATE);
        }
        return rate == USB_AUDIO_RATE;
    }
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    if (TU_U16_HIGH(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        uint8_t freq[3] = {
            (uint8_t)(USB_AUDIO_RATE & 0xFFU),
            (uint8_t)((USB_AUDIO_RATE >> 8) & 0xFFU),
            (uint8_t)((USB_AUDIO_RATE >> 16) & 0xFFU),
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;
    const uint8_t channel = TU_U16_LOW(p_request->wValue);
    const uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
    if (TU_U16_HIGH(p_request->wIndex) != UAC1_ENTITY_FEATURE_UNIT || channel > 2U ||
        p_request->bRequest != AUDIO10_CS_REQ_SET_CUR) {
        return false;
    }

    if (ctrl == AUDIO10_FU_CTRL_MUTE && p_request->wLength == 1U) {
        s_mute[channel] = buf[0];
    } else if (ctrl == AUDIO10_FU_CTRL_VOLUME && p_request->wLength == 2U) {
        int32_t volume = (int16_t)tu_unaligned_read16(buf);
        if (volume < VOLUME_MIN) {
            volume = VOLUME_MIN;
        } else if (volume > VOLUME_MAX) {
            volume = VOLUME_MAX;
        }
        s_volume[channel] = (int16_t)volume;
    } else {
        return false;
    }
    update_gains();
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    const uint8_t channel = TU_U16_LOW(p_request->wValue);
    const uint8_t ctrl = TU_U16_HIGH(p_request->wValue);
    if (TU_U16_HIGH(p_request->wIndex) != UAC1_ENTITY_FEATURE_UNIT || channel > 2U) {
        return false;
    }

    if (ctrl == AUDIO10_FU_CTRL_MUTE && p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_mute[channel], 1);
    }
    if (ctrl == AUDIO10_FU_CTRL_VOLUME) {
        int16_t value;
        switch (p_request->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR: value = s_volume[channel]; break;
        case AUDIO10_CS_REQ_GET_MIN: value = VOLUME_MIN; break;
        case AUDIO10_CS_REQ_GET_MAX: value = VOLUME_MAX; break;
        case AUDIO10_CS_REQ_GET_RES: value = VOLUME_RES; break;
        default: return false;
        }
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &value, sizeof(value));
    }
    return false;
}

/*
 * Feedback by FIFO level: TinyUSB averages the receive FIFO count and tells
 * the host to speed up or slow down until it sits at half. The capture loop
 * empties the FIFO at the I2S rate, so the host ends up at that rate.
 */
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = USB_AUDIO_RATE;
}

/* ---- Capture hook ---- */

bool usb_audio_fill_stereo(int16_t *stereo, size_t frames)
{
    if (!s_started || stereo == NULL || frames == 0U || frames > USB_AUDIO_MAX_FRAMES) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (!s_streaming) {
        s_prebuffering = true;
        s_active = false;
        s_last_signal_us = 0;
        return false;
    }

    const uint32_t frame_bytes = (uint32_t)(frames * USB_AUDIO_FRAME_BYTES);
    const uint32_t nominal = FIFO_BYTES / 2U + frame_bytes / 2U;
    const uint32_t avail = tud_audio_available();
    if (s_prebuffering) {
        if (avail < nominal) {
            /* Still playing (a gap within the release time): silence, not
             * a jump back to the I2S input. */
            s_active = s_last_signal_us != 0 && now - s_last_signal_us < USB_AUDIO_RELEASE_US;
            if (s_active) {
                memset(stereo, 0, frame_bytes);
            }
            return s_active;
        }
        s_prebuffering = false;
    }

    uint32_t dropped = 0;
    uint32_t repeated = 0;
    size_t take = frames;
    if (avail > nominal + SLIP_MARGIN_BYTES) {
        (void)tud_audio_read(s_scratch, USB_AUDIO_FRAME_BYTES);
        dropped = 1;
    } else if (avail + SLIP_MARGIN_BYTES < nominal) {
        take = frames - 1U;
        repeated = 1;
    }

    size_t got = tud_audio_read(s_scratch, (uint16_t)(take * USB_AUDIO_FRAME_BYTES)) / USB_AUDIO_FRAME_BYTES;
    if (repeated != 0U && got == take && got > 0U) {
        s_scratch[2U * got] = s_scratch[2U * got - 2U];
        s_scratch[2U * got + 1U] = s_scratch[2U * got - 1U];
        ++got;
    }
    uint32_t underrun = 0;
    if (got < frames) {
        memset(&s_scratch[2U * got], 0, (frames - got) * USB_AUDIO_FRAME_BYTES);
        underrun = (uint32_t)(frames - got);
        s_prebuffering = true;
    }

    const float gain_l = s_gain[0];
    const float gain_r = s_gain[1];
    int32_t peak = 0;
    for (size_t i = 0; i < 2U * frames; ++i) {
        const int32_t v = s_scratch[i];
        const int32_t a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > SIGNAL_PEAK) {
        s_last_signal_us = now;
    }
    s_active = s_last_signal_us != 0 && now - s_last_signal_us < USB_AUDIO_RELEASE_US;

    if (dropped != 0U || repeated != 0U || underrun != 0U) {
        portENTER_CRITICAL(&s_stats_lock);
        s_dropped_frames += dropped;
        s_repeated_frames += repeated;
        s_underrun_frames += underrun;
        portEXIT_CRITICAL(&s_stats_lock);
    }

    if (!s_active) {
        return false;
    }
    if (gain_l == 1.0f && gain_r == 1.0f) {
        memcpy(stereo, s_scratch, frames * USB_AUDIO_FRAME_BYTES);
    } else {
        for (size_t i = 0; i < frames; ++i) {
            stereo[2U * i] = (int16_t)lrintf((float)s_scratch[2U * i] * gain_l);
            stereo[2U * i + 1U] = (int16_t)lrintf((float)s_scratch[2U * i + 1U] * gain_r);
        }
    }
    return true;
}

void usb_audio_take_stats(usb_audio_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_started) {
        return;
    }
    out->mounted = s_mounted;
    out->streaming = s_streaming;
    out->active = s_active;
    out->fill_bytes = tud_audio_available();
    out->fifo_bytes = FIFO_BYTES;
    out->volume_db = (int16_t)(s_volume[0] / 256);
    out->muted = s_mute[0] != 0U;
    portENTER_CRITICAL(&s_stats_lock);
    out->underrun_frames = s_underrun_frames;
    out->dropped_frames = s_dropped_frames;
    out->repeated_frames = s_repeated_frames;
    s_underrun_frames = 0;
    s_dropped_frames = 0;
    s_repeated_frames = 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

/* ---- Start, PHY ---- */

static void usb_task(void *arg)
{
    (void)arg;
    const tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    /* Here, on core 0: the USB interrupt is allocated on the calling core. */
    if (!tusb_init(0, &dev_init)) {
        ESP_LOGE(TAG, "TinyUSB init failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        tud_task();
    }
}

esp_err_t usb_audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_scratch = heap_caps_malloc(USB_AUDIO_MAX_FRAMES * USB_AUDIO_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_scratch == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_product, sizeof(s_product), "SnapMesh %02X%02X", mac[4], mac[5]);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /*
     * Take USB-Serial-JTAG off the bus first, so the host sees its port go
     * away before the sound card appears. Switched straight over, D+ can
     * stay high throughout and the host keeps the old device (seen after
     * esptool's reset on 2026-10-08). The override stays set while OTG
     * owns the pads; route_phy_to_serial_jtag() clears it.
     */
    const usb_serial_jtag_pull_override_vals_t detached = { .dp_pd = true, .dm_pd = true };
    usb_serial_jtag_ll_phy_enable_pull_override(&detached);
    vTaskDelay(pdMS_TO_TICKS(200));

    const usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    esp_err_t result = usb_new_phy(&phy_conf, &s_phy);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY failed: %s", esp_err_to_name(result));
        return result;
    }

    s_started = true;
    if (xTaskCreatePinnedToCore(usb_task, "tinyusb", USB_TASK_STACK, NULL, USB_TASK_PRIORITY, NULL, 0) !=
        pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "USB sound card \"%s\" on the native port (48 kHz, stereo, 16 bit); "
             "log and flashing now via COM", s_product);
    return ESP_OK;
}

static void route_phy_to_serial_jtag(void)
{
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    /*
     * USJ's own pull-up again, see usb_audio_start(). The pull bits go back
     * to their reset values (D+ pull-up, no pull-downs) before the override
     * is dropped: left as set there, the board vanished from the bus in
     * download mode (B688, 2026-10-08), and a software restart does not
     * reset USJ.
     */
    const usb_serial_jtag_pull_override_vals_t reset_values = { .dp_pu = true };
    usb_serial_jtag_ll_phy_enable_pull_override(&reset_values);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

void usb_audio_release_phy(void)
{
    if (RTCCNTL.usb_conf.sw_hw_usb_phy_sel != 0U && RTCCNTL.usb_conf.sw_usb_phy_sel != 0U) {
        route_phy_to_serial_jtag();
        ESP_LOGW(TAG, "Native USB port was still on USB OTG from the last run; back to USB-Serial-JTAG");
    }
}

void usb_audio_restart_to_download(void)
{
    ESP_LOGW(TAG, "Restarting into download mode on the native USB port");
    /*
     * The host has to see the sound card go away first. Handed straight to
     * USB-Serial-JTAG, D+ never drops -- USJ pulls it up as OTG did, and a
     * software restart does not reset USJ -- so the host keeps the old
     * device and never enumerates the ROM's serial port (B688, 2026-10-08).
     */
    if (s_started) {
        (void)tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    route_phy_to_serial_jtag();
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
