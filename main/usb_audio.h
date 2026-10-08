/**
 * @file usb_audio.h
 * @brief USB sound card on the server's native USB port: a USB Audio
 *        Class 1 speaker, 48 kHz, stereo, 16 bit, with feedback endpoint.
 *
 * A PC plays into it like into any USB speaker, no driver needed (Windows,
 * Linux, macOS bring UAC1 drivers). The audio replaces the I2S input while
 * the PC is playing, see usb_audio_fill_stereo(). The server's I2S clock
 * stays in charge: the capture loop empties the receive FIFO at that rate,
 * and the feedback endpoint tells the PC to send exactly as fast.
 *
 * While it runs, the native port is no longer USB-Serial-JTAG: logging and
 * flashing go through the COM port (UART0), or through
 * usb_audio_restart_to_download().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts TinyUSB as the speaker. Server role only, once, before audio. */
esp_err_t usb_audio_start(void);

/*
 * Gives the native port back to USB-Serial-JTAG if a previous run left it
 * on USB OTG. The routing is kept in an RTC register, which a software
 * restart does not reset. Called on every boot without USB audio.
 */
void usb_audio_release_phy(void);

/*
 * Capture hook, called once per frame by the server's capture loop, at the
 * I2S rate. Always takes `frames` stereo frames out of the receive FIFO
 * while the PC streams -- that is what holds the PC to the server's clock --
 * but only copies them over `stereo` (interleaved L/R, the I2S frame just
 * read) while the PC is actually playing. Returns true when it did.
 */
bool usb_audio_fill_stereo(int16_t *stereo, size_t frames);

/* Counters for the server's stats line; the event counts clear on read. */
typedef struct {
    bool mounted;              /* enumerated by a host */
    bool streaming;            /* host has the audio interface open */
    bool active;               /* replacing the I2S input right now */
    uint32_t fill_bytes;       /* receive FIFO level now */
    uint32_t fifo_bytes;       /* receive FIFO size */
    uint32_t underrun_frames;  /* frames filled with silence */
    uint32_t dropped_frames;   /* emergency: FIFO near full, frame skipped */
    uint32_t repeated_frames;  /* emergency: FIFO near empty, frame doubled */
    int16_t volume_db;         /* host's master volume */
    bool muted;                /* host's master mute */
} usb_audio_stats_t;

void usb_audio_take_stats(usb_audio_stats_t *out);

/*
 * Restarts into the ROM download mode on USB-Serial-JTAG, so esptool can
 * flash through the native port without holding BOOT (connect with
 * --before no_reset). Works whether or not USB audio is running. Does not
 * return.
 */
void usb_audio_restart_to_download(void);

#ifdef __cplusplus
}
#endif
