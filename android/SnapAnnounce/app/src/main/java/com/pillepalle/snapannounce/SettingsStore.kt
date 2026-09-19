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
     * MediaRecorder.AudioSource to record from. Phones differ a lot here:
     * on a Galaxy A56 VOICE_COMMUNICATION delivered speech at only -35 to
     * -45 dBFS, which needs so much gain that the room and the speakers'
     * own delayed output come up with it. Selectable so the best one for a
     * given phone can be found by ear.
     */
    var micSource: Int
        get() = prefs.getInt(KEY_MIC_SOURCE, MediaRecorder.AudioSource.VOICE_COMMUNICATION)
        set(value) = prefs.edit().putInt(KEY_MIC_SOURCE, value).apply()

    /**
     * Upper limit of the automatic gain, in dB. The trade-off is set by ear:
     * more means louder, but also more room echo and feedback from nearby
     * speakers.
     */
    var maxGainDb: Int
        get() = prefs.getInt(KEY_MAX_GAIN, DEFAULT_MAX_GAIN_DB)
        set(value) = prefs.edit().putInt(KEY_MAX_GAIN, value).apply()

    companion object {
        private const val PREFS_NAME = "snapannounce"
        private const val KEY_HOST = "server_host"
        private const val KEY_MIC_SOURCE = "mic_source"
        private const val KEY_MAX_GAIN = "max_gain_db"

        const val DEFAULT_MAX_GAIN_DB = 24
        const val MAX_GAIN_LIMIT_DB = 42

        const val DEFAULT_HOST = "192.168.5.1"

        /** snapcontrol.c, JSON-RPC: Voice.Start / Voice.Stop. */
        const val CONTROL_PORT = 1705

        /** voice_announce.c, raw PCM datagrams while armed. */
        const val VOICE_PORT = 1706
    }
}
