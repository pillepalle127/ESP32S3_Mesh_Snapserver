package com.pillepalle.snapannounce

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Binder
import android.os.IBinder
import android.os.Process
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import kotlin.math.log10

enum class AnnounceState { IDLE, CONNECTING, ON_AIR, ERROR }

data class UiState(
    val state: AnnounceState = AnnounceState.IDLE,
    val elapsedSeconds: Int = 0,
    val message: String? = null,
)

/**
 * Captures the microphone and streams it to the ESP32 server as long as an
 * announcement is armed.
 *
 * A foreground service rather than work inside the Activity: the button is
 * latching, not push-to-talk, so an announcement is expected to outlive a
 * locked screen or a quick switch to another app.
 *
 * One coroutine runs a whole announcement top to bottom -- connect, arm,
 * capture until asked to stop, disarm, clean up. Everything that wants an
 * announcement to end (the button, the notification action, a lost Wi-Fi
 * network) only flips [capturing] and records why; that coroutine notices
 * within one 10 ms read and does the disarm and cleanup itself. That keeps
 * Voice.Stop on the one code path that also owns the control connection,
 * instead of racing a second coroutine for it.
 *
 * Wire format towards the firmware (voice_announce.c): one UDP datagram per
 * Opus packet (normally 20 ms), a 4-byte little-endian sequence number
 * followed by the raw Opus packet, 16 kHz mono speech.
 */
class VoiceAnnounceService : Service() {

    inner class LocalBinder : Binder() {
        fun service(): VoiceAnnounceService = this@VoiceAnnounceService
    }

    private val binder = LocalBinder()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private val _uiState = MutableStateFlow(UiState())
    val uiState: StateFlow<UiState> = _uiState.asStateFlow()

    private lateinit var settings: SettingsStore
    private lateinit var connectivity: ConnectivityManager

    private var announceJob: Job? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    @Volatile private var capturing = false
    @Volatile private var stopReason: String? = null

    override fun onCreate() {
        super.onCreate()
        settings = SettingsStore(this)
        connectivity = getSystemService(ConnectivityManager::class.java)
        createNotificationChannel()
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> startAnnouncement()
            ACTION_STOP -> requestStop("Durchsage beendet")
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        capturing = false
        unregisterNetworkCallback()
        scope.cancel()
        super.onDestroy()
    }

    private fun startAnnouncement() {
        val running = announceJob?.isActive == true

        // Must happen on every ACTION_START, running or not:
        // startForegroundService() gives us only a few seconds to get here.
        // Text matches the current state so a repeated START doesn't reset
        // an "on air" notification back to "connecting".
        val text = if (running && _uiState.value.state == AnnounceState.ON_AIR) {
            "Durchsage läuft"
        } else {
            "Verbinde…"
        }
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(text, showStop = true),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
        )

        if (running) return

        stopReason = null
        capturing = false
        _uiState.value = UiState(state = AnnounceState.CONNECTING)
        announceJob = scope.launch { runAnnouncement() }
    }

    /** Asks the running announcement to end; see the class comment. */
    private fun requestStop(reason: String?) {
        stopReason = reason
        capturing = false
        if (announceJob?.isActive != true) {
            // Nothing running (e.g. STOP arrived for a service that was only
            // bound): just make sure we don't linger as a started service.
            finish(UiState(state = AnnounceState.IDLE))
        }
    }

    private suspend fun runAnnouncement() {
        val host = settings.serverHost

        val network = wifiNetworkOrNull()
        if (network == null) {
            finish(UiState(state = AnnounceState.ERROR, message = "Kein WLAN verbunden"))
            return
        }

        val rpc = try {
            withContext(Dispatchers.IO) {
                RpcClient.connect(
                    host,
                    SettingsStore.CONTROL_PORT,
                    CONNECT_TIMEOUT_MS,
                    READ_TIMEOUT_MS,
                    network,
                )
            }
        } catch (e: IOException) {
            finish(UiState(state = AnnounceState.ERROR, message = "Server $host nicht erreichbar"))
            return
        }

        // Arm first, then open the microphone: capturing before the server
        // is armed would only produce a backlog that the firmware's small
        // mailbox then carries as permanent extra latency.
        // null = armed, otherwise why not. The firmware answers a refusal
        // with result.busy; an error means it doesn't know Voice.Start at all.
        val armed = try {
            val response = withContext(Dispatchers.IO) { rpc.call("Voice.Start") }
            Log.i(TAG, "Voice.Start -> $response")
            val result = response.optJSONObject("result")
            when {
                result?.optBoolean("active") == true -> null
                result?.optBoolean("busy") == true -> "Es läuft bereits eine andere Durchsage"
                response.has("error") -> "Server-Firmware kennt keine Durchsagen"
                else -> "Unerwartete Antwort vom Server"
            }
        } catch (e: IOException) {
            "Server antwortet nicht"
        } catch (e: org.json.JSONException) {
            "Unerwartete Antwort vom Server"
        }

        if (armed != null || stopReason != null) {
            // Either arming failed, or the user pressed stop while we were
            // still connecting -- disarm (harmless if never armed) and leave.
            withContext(Dispatchers.IO + NonCancellable) {
                if (armed == null) {
                    try {
                        rpc.call("Voice.Stop")
                    } catch (_: IOException) {
                    }
                }
                rpc.close()
            }
            finish(
                if (armed != null) UiState(state = AnnounceState.ERROR, message = armed)
                else UiState(state = AnnounceState.IDLE, message = stopReason)
            )
            return
        }

        watchNetworkLoss(network)
        capturing = true
        _uiState.value = UiState(state = AnnounceState.ON_AIR)
        updateNotification("Durchsage läuft", showStop = true)

        val ticker = scope.launch {
            var seconds = 0
            while (isActive) {
                delay(1000)
                seconds++
                _uiState.update { it.copy(elapsedSeconds = seconds) }
            }
        }

        val captureError = withContext(Dispatchers.IO) {
            captureAndSend(host, network, settings.micSource, settings.maxGainDb)
        }
        ticker.cancel()
        capturing = false

        // Best effort: if this is lost, the firmware's silence watchdog and
        // the connection close right after end the announcement anyway.
        withContext(Dispatchers.IO + NonCancellable) {
            try {
                rpc.call("Voice.Stop")
            } catch (_: IOException) {
            }
            rpc.close()
        }

        unregisterNetworkCallback()
        finish(
            if (captureError != null) UiState(state = AnnounceState.ERROR, message = captureError)
            else UiState(state = AnnounceState.IDLE, message = stopReason)
        )
    }

    /**
     * Blocking capture-and-send loop. Returns null when it ended because
     * [capturing] was cleared, or an error message if it could not run.
     */
    @SuppressLint("MissingPermission") // checked by MainActivity before starting
    private fun captureAndSend(host: String, network: Network, micSource: Int, maxGainDb: Int): String? {
        val minBuffer = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING)
        if (minBuffer <= 0) return "Mikrofon unterstützt 16 kHz nicht"

        val recorder = try {
            AudioRecord(
                // Chosen in the app, see SettingsStore.micSource.
                micSource,
                SAMPLE_RATE,
                CHANNEL,
                ENCODING,
                maxOf(minBuffer, FRAME_BYTES * 4),
            )
        } catch (e: SecurityException) {
            return "Mikrofon-Berechtigung fehlt"
        }

        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            recorder.release()
            return "Mikrofon konnte nicht geöffnet werden"
        }

        val socket = try {
            DatagramSocket().also { network.bindSocket(it) }
        } catch (e: IOException) {
            recorder.release()
            return "UDP-Socket konnte nicht geöffnet werden"
        }

        val address = try {
            network.getByName(host)
        } catch (e: IOException) {
            socket.close()
            recorder.release()
            return "Adresse $host ungültig"
        }

        val encoder = try {
            OpusEncoder(SAMPLE_RATE, OPUS_BITRATE)
        } catch (e: Exception) {
            socket.close()
            recorder.release()
            Log.w(TAG, "No Opus encoder", e)
            return "Kein Opus-Encoder auf diesem Gerät (Android 10 nötig)"
        }

        val samples = ShortArray(FRAME_SAMPLES)
        val packet = ByteArray(HEADER_BYTES + MAX_OPUS_PACKET)
        val datagram = DatagramPacket(packet, packet.size, address, SettingsStore.VOICE_PORT)
        var sequence = 0
        var frames = 0

        // Diagnostics, logged every STATS_FRAMES: without them "the speakers
        // stay silent" can't be told apart from "nothing left the phone".
        var sent = 0
        var failed = 0
        var sentBytes = 0L
        var micPeak = 0
        var outPeak = 0
        var outSumSquares = 0.0
        var outSamples = 0
        var lastError: String? = null
        val agc = VoiceAgc(maxGainDb = maxGainDb.toDouble(),
                           startGainDb = minOf(20, maxGainDb).toDouble())
        Log.i(TAG, "Streaming Opus to $address:${SettingsStore.VOICE_PORT} " +
            "source=$micSource max_gain=$maxGainDb dB")

        val oldPriority = Process.getThreadPriority(Process.myTid())
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)

        try {
            recorder.startRecording()
            if (recorder.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                return "Aufnahme konnte nicht gestartet werden"
            }

            while (capturing) {
                // 10 ms per read; the encoder collects them into its own
                // frames and hands back a packet as soon as one is complete.
                var filled = 0
                while (filled < FRAME_SAMPLES && capturing) {
                    val n = recorder.read(samples, filled, FRAME_SAMPLES - filled, AudioRecord.READ_BLOCKING)
                    if (n < 0) return "Lesefehler vom Mikrofon ($n)"
                    filled += n
                }
                if (!capturing) break
                frames++

                micPeak = maxOf(micPeak, peakOf(samples))
                agc.process(samples)
                outPeak = maxOf(outPeak, peakOf(samples))
                for (s in samples) {
                    val v = s.toDouble()
                    outSumSquares += v * v
                }
                outSamples += samples.size

                encoder.encode(samples) { payload, length ->
                    // Wire format, see voice_announce.c: 4-byte little-endian
                    // sequence number, then the Opus packet.
                    val seq = sequence++
                    packet[0] = seq.toByte()
                    packet[1] = (seq ushr 8).toByte()
                    packet[2] = (seq ushr 16).toByte()
                    packet[3] = (seq ushr 24).toByte()
                    System.arraycopy(payload, 0, packet, HEADER_BYTES, length)
                    datagram.length = HEADER_BYTES + length
                    try {
                        socket.send(datagram)
                        sent++
                        sentBytes += HEADER_BYTES + length
                    } catch (e: IOException) {
                        // A lost datagram is a gap of one frame; never wait or
                        // retry, same contract as the firmware side.
                        failed++
                        lastError = e.toString()
                    }
                }

                if (frames % STATS_FRAMES == 0) {
                    val seconds = STATS_FRAMES * FRAME_SAMPLES.toDouble() / SAMPLE_RATE
                    Log.i(
                        TAG,
                        ("sent=$sent failed=$failed rate=%.0f kbit/s mic_peak=%.1f dBFS " +
                            "out_peak=%.1f dBFS out_rms=%.1f dBFS gain=%.1f dB%s").format(
                            sentBytes * 8 / seconds / 1000.0,
                            dbfs(micPeak),
                            dbfs(outPeak),
                            if (outSamples > 0) {
                                val rms = kotlin.math.sqrt(outSumSquares / outSamples)
                                if (rms > 0.0) 20.0 * log10(rms / 32767.0) else -120.0
                            } else {
                                -120.0
                            },
                            agc.gainDb,
                            lastError?.let { " last_error=$it" } ?: "",
                        ),
                    )
                    sentBytes = 0
                    micPeak = 0
                    outPeak = 0
                    outSumSquares = 0.0
                    outSamples = 0
                    lastError = null
                }
            }
            return null
        } finally {
            try {
                recorder.stop()
            } catch (_: IllegalStateException) {
            }
            recorder.release()
            encoder.release()
            socket.close()
            Process.setThreadPriority(oldPriority)
        }
    }

    private fun peakOf(samples: ShortArray): Int {
        var peak = 0
        for (s in samples) {
            val a = if (s < 0) -s.toInt() else s.toInt()
            if (a > peak) peak = a
        }
        return peak
    }

    private fun dbfs(peak: Int): Double =
        if (peak > 0) 20.0 * log10(peak / 32767.0) else -120.0

    private fun wifiNetworkOrNull(): Network? =
        connectivity.allNetworks.firstOrNull { net ->
            connectivity.getNetworkCapabilities(net)
                ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }

    /**
     * If the Wi-Fi the announcement runs on goes away, stop -- rather than
     * let the UI keep claiming "on air" while datagrams go nowhere. The
     * sockets are bound to this network, so Android cannot silently move
     * them to mobile data either.
     */
    private fun watchNetworkLoss(network: Network) {
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onLost(lost: Network) {
                if (lost == network) requestStop("WLAN-Verbindung verloren")
            }
        }
        networkCallback = callback
        connectivity.registerNetworkCallback(request, callback)
    }

    private fun unregisterNetworkCallback() {
        networkCallback?.let {
            try {
                connectivity.unregisterNetworkCallback(it)
            } catch (_: IllegalArgumentException) {
            }
        }
        networkCallback = null
    }

    private fun finish(state: UiState) {
        _uiState.value = state
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Durchsage",
            NotificationManager.IMPORTANCE_LOW,
        )
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(text: String, showStop: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(open)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)

        if (showStop) {
            val stop = PendingIntent.getService(
                this,
                1,
                Intent(this, VoiceAnnounceService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            builder.addAction(0, "Beenden", stop)
        }
        return builder.build()
    }

    private fun updateNotification(text: String, showStop: Boolean) {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(text, showStop))
    }

    companion object {
        private const val TAG = "SnapAnnounce"

        /** Log line every 2 s of audio (10 ms frames). */
        private const val STATS_FRAMES = 200

        const val ACTION_START = "com.pillepalle.snapannounce.action.START"
        const val ACTION_STOP = "com.pillepalle.snapannounce.action.STOP"

        private const val CHANNEL_ID = "voice_announce"
        private const val NOTIFICATION_ID = 1

        /** How long reaching the server may take before "not reachable". */
        private const val CONNECT_TIMEOUT_MS = 1500

        /** Waiting for an RPC answer. Longer than the connect: the server's
         *  control task can be slow to answer while the mesh is busy. */
        private const val READ_TIMEOUT_MS = 5000

        /*
         * 16 kHz, not 48: at 48 kHz and 32 kbit/s Opus picks its hybrid mode
         * (SILK and CELT at once), the most expensive one to decode. On the
         * server that took ~6 ms of CPU per 20 ms packet next to the music
         * encoder -- core 1 ran at 97 % and the announcement stuttered
         * (2026-09-19). Wideband speech is pure SILK, several times cheaper,
         * and 8 kHz of audio bandwidth is plenty for announcements. The
         * ESP32s still decode to 48 kHz; Opus resamples internally.
         */
        private const val SAMPLE_RATE = 16000
        private const val CHANNEL = AudioFormat.CHANNEL_IN_MONO
        private const val ENCODING = AudioFormat.ENCODING_PCM_16BIT

        /** Microphone reads of 10 ms; the encoder makes its own frames. */
        private const val FRAME_SAMPLES = SAMPLE_RATE / 100
        private const val FRAME_BYTES = FRAME_SAMPLES * 2

        /** Wire format, see voice_announce.c. */
        private const val HEADER_BYTES = 4
        private const val MAX_OPUS_PACKET = 1275

        /** Wideband speech is clear at this rate; raw PCM was 768 kbit/s. */
        private const val OPUS_BITRATE = 24000
    }
}
