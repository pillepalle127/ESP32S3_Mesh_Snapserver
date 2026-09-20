package com.pillepalle.snapannounce

import android.content.Context
import android.media.MediaRecorder

/**
 * The one setting this app has: which ESP32 to talk to.
 *
 * Defaults to 192.168.5.1 -- PROVISIONING_AP_IP_ADDR on the firmware side,
 * which only the server (mesh root) pins its AP to. Relay nodes get their
 * own subnet from esp_bridge's segment-conflict check and forward
 * 192.168.5.1 upstream via NAPT, so this address reaches the server from
 * any SnapMesh access point -- directly on the root's AP it is just one hop
 * shorter.
 */
class SettingsStore(context: Context) {

    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    var serverHost: String
        get() = prefs.getString(KEY_HOST, DEFAULT_HOST) ?: DEFAULT_HOST
        set(value) = prefs.edit().putString(KEY_HOST, value.trim()).apply()

    /**
     * MediaRecorder.AudioSource to record from. Phones differ a lot here.
     * MIC by default: on a Galaxy A56 VOICE_COMMUNICATION delivered speech
     * at only -35 to -45 dBFS -- it is the uplink of a phone call, with the
     * vendor's own noise suppression and gain control tuned for a handset
     * held to the ear -- while MIC reached -2 to -5 dBFS. Its echo
     * cancellation, the original reason for picking it, only knows the
     * phone's own playback and does nothing about an external speaker.
     *
     * Still selectable: another phone may behave differently, and in a
     * reverberant room VOICE_COMMUNICATION's noise suppression can win
     * despite the level.
     */
    var micSource: Int
        get() = prefs.getInt(KEY_MIC_SOURCE, MediaRecorder.AudioSource.MIC)
        set(value) = prefs.edit().putInt(KEY_MIC_SOURCE, value).apply()

    /**
     * Upper limit of the automatic gain, in dB. The trade-off is set by ear:
     * more means louder, but also more room echo and feedback from nearby
     * speakers.
     */
    var maxGainDb: Int
        get() = prefs.getInt(KEY_MAX_GAIN, DEFAULT_MAX_GAIN_DB)
        set(value) = prefs.edit().putInt(KEY_MAX_GAIN, value).apply()

    /**
     * Level the automatic gain aims for, in dBFS RMS. This is what decides
     * whether the announcement is as loud as the music: measured on device,
     * music reaches the clients at -24 to -28 dBFS RMS. Higher means louder
     * but more compressed.
     */
    var targetRmsDbfs: Int
        get() = prefs.getInt(KEY_TARGET_RMS, DEFAULT_TARGET_RMS_DBFS)
        set(value) = prefs.edit().putInt(KEY_TARGET_RMS, value).apply()

    companion object {
        private const val PREFS_NAME = "snapannounce"
        private const val KEY_HOST = "server_host"
        private const val KEY_MIC_SOURCE = "mic_source"
        private const val KEY_MAX_GAIN = "max_gain_db"
        private const val KEY_TARGET_RMS = "target_rms_dbfs"

        const val DEFAULT_MAX_GAIN_DB = 24
        const val MAX_GAIN_LIMIT_DB = 42

        const val DEFAULT_TARGET_RMS_DBFS = -12
        const val MIN_TARGET_RMS_DBFS = -30
        const val MAX_TARGET_RMS_DBFS = -6

        const val DEFAULT_HOST = "192.168.5.1"

        /** snapcontrol.c, JSON-RPC: Voice.Start / Voice.Stop. */
        const val CONTROL_PORT = 1705

        /** voice_announce.c, raw PCM datagrams while armed. */
        const val VOICE_PORT = 1706
    }
}
