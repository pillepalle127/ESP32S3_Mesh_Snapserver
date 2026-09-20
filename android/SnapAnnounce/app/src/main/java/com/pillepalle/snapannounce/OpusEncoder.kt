package com.pillepalle.snapannounce

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import java.nio.ByteOrder

/**
 * Android's built-in Opus encoder (MediaCodec, Android 10+), mono, at the
 * caller's sample rate, wrapped for a synchronous capture loop.
 *
 * Opus rather than raw PCM: at 768 kbit/s per level-1 client, raw PCM
 * needed about nine times the music's airtime and congested the mesh --
 * see voice_announce.c. The ESP32s decode with the same libopus they use
 * for the music.
 *
 * Every output buffer from MediaCodec's Opus encoder is exactly one raw
 * Opus packet (no container), which is what goes on the wire. The first
 * few are codec-specific data (OpusHead, pre-skip, seek pre-roll), flagged
 * BUFFER_FLAG_CODEC_CONFIG; a decoder that is fed packets directly doesn't
 * need them, so they are skipped.
 *
 * Never blocks the capture thread for long: if the encoder has no free
 * input buffer within ENCODER_WAIT_US, that 10 ms is dropped rather than
 * waited for -- the same "drop, don't wait" rule as the rest of the path.
 */
class OpusEncoder(private val sampleRate: Int, bitrate: Int) {

    private val codec: MediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
    private val info = MediaCodec.BufferInfo()
    private val out = ByteArray(1275)
    private var samplesIn = 0L
    private var samplesOut = 0L

    /**
     * Audio handed in but not yet handed back, in milliseconds -- the delay
     * the encoder itself adds. Part of it is Opus' own frame and lookahead,
     * the rest is however much MediaCodec keeps in flight, which is not
     * documented and differs between devices. Measured because it sits in
     * the announcement's end-to-end latency, which is the whole point of
     * this path.
     */
    val backlogMs: Double
        get() = (samplesIn - samplesOut) * 1000.0 / sampleRate

    init {
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, sampleRate, 1).apply {
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 480 * 2)
        }
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
        } catch (e: Exception) {
            codec.release()
            throw e
        }
    }

    /**
     * Queues [pcm] (native-endian 16-bit mono) and hands every finished
     * packet to [onPacket] as (buffer, length). The buffer is reused.
     */
    fun encode(pcm: ShortArray, onPacket: (ByteArray, Int) -> Unit) {
        val inIndex = codec.dequeueInputBuffer(ENCODER_WAIT_US)
        if (inIndex >= 0) {
            val input = codec.getInputBuffer(inIndex)
            if (input != null) {
                input.clear()
                input.order(ByteOrder.nativeOrder()).asShortBuffer().put(pcm)
                val ptsUs = samplesIn * 1_000_000L / sampleRate
                codec.queueInputBuffer(inIndex, 0, pcm.size * 2, ptsUs, 0)
                samplesIn += pcm.size
            }
        }

        while (true) {
            val outIndex = codec.dequeueOutputBuffer(info, 0)
            if (outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue
            if (outIndex < 0) break

            val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
            if (!isConfig) {
                /* One packet is one Opus frame; 20 ms is what this encoder
                 * produces at every rate we use. */
                samplesOut += sampleRate / 50
            }
            if (!isConfig && info.size in 1..out.size) {
                val output = codec.getOutputBuffer(outIndex)
                if (output != null) {
                    output.position(info.offset)
                    output.limit(info.offset + info.size)
                    output.get(out, 0, info.size)
                    onPacket(out, info.size)
                }
            }
            codec.releaseOutputBuffer(outIndex, false)
        }
    }

    fun release() {
        try {
            codec.stop()
        } catch (_: IllegalStateException) {
        }
        codec.release()
    }

    private companion object {
        const val ENCODER_WAIT_US = 5_000L
    }
}
