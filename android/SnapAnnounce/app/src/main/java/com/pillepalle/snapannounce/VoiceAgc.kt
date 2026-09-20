package com.pillepalle.snapannounce

import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sign
import kotlin.math.sqrt
import kotlin.math.tanh

/**
 * Makes the announcement loud enough to stand next to music.
 *
 * VOICE_COMMUNICATION is tuned for a phone call: on a Galaxy A56 speech
 * peaked at -32..-41 dBFS. Lifting the *peaks* to -3 dBFS (the first
 * version of this) was still too quiet by ear: speech has a crest factor of
 * 15-20 dB, mastered music maybe 8-10, so at equal peaks the voice's
 * average level -- what loudness is -- sits well below the music.
 *
 * So this works like broadcast processing: an AGC levels the *average*
 * (RMS per 10 ms frame) to targetRmsDbfs, and a soft limiter behind it
 * catches the peaks that then stick out above full scale.
 *  - Gain falls fast (attackDbPerFrame) on a loud frame and rises slowly
 *    (releaseDbPerFrame), ramped across each frame so it doesn't click.
 *  - Frames below gateDbfs (pauses) leave the gain alone, so the noise floor
 *    isn't pumped up between words.
 *  - It starts at startGainDb, so the first words aren't quiet while the
 *    gain is still climbing.
 *
 * Both knobs are set by ear in the app (SettingsStore), because the right
 * values depend on the phone's microphone and on how loud the music is:
 *  - targetRmsDbfs decides how loud the announcement is next to the music.
 *    Measured on device (2026-09-20): music arrives at the clients at -24
 *    to -28 dBFS RMS, so the announcement has to land in that region.
 *  - maxGainDb caps how far a quiet microphone may be lifted. Too much and
 *    the phone picks up the room and the speakers' own delayed output
 *    nearly as strongly as the voice -- audible as echo. -14 dBFS with
 *    +42 dB was tried and overdrove badly.
 */
class VoiceAgc(
    targetRmsDbfs: Double = -12.0,
    private val maxGainDb: Double = 24.0,
    gateDbfs: Double = -60.0,
    private val attackDbPerFrame: Double = 3.0,
    private val releaseDbPerFrame: Double = 0.1,   // 10 dB/s
    startGainDb: Double = 20.0,
    limitThresholdDbfs: Double = -6.0,
    limitCeilingDbfs: Double = -0.5,
) {
    private val targetRms = FULL_SCALE * 10.0.pow(targetRmsDbfs / 20.0)
    private val gate = FULL_SCALE * 10.0.pow(gateDbfs / 20.0)
    private val threshold = FULL_SCALE * 10.0.pow(limitThresholdDbfs / 20.0)
    private val ceiling = FULL_SCALE * 10.0.pow(limitCeilingDbfs / 20.0)

    /** Current gain in dB, for the diagnostics line. */
    var gainDb: Double = startGainDb
        private set

    /** Scales and limits [samples] in place. */
    fun process(samples: ShortArray) {
        var sumSquares = 0.0
        for (s in samples) {
            val v = s.toDouble()
            sumSquares += v * v
        }
        val rms = sqrt(sumSquares / samples.size)

        val previousDb = gainDb
        if (rms > gate) {
            val wantedDb = min(maxGainDb, 20.0 * log10(targetRms / rms))
            gainDb = if (wantedDb < gainDb) {
                max(wantedDb, gainDb - attackDbPerFrame)
            } else {
                min(wantedDb, gainDb + releaseDbPerFrame)
            }
        }

        val g0 = 10.0.pow(previousDb / 20.0)
        val g1 = 10.0.pow(gainDb / 20.0)
        val step = (g1 - g0) / samples.size
        for (i in samples.indices) {
            val g = g0 + step * (i + 1)
            samples[i] = limit(samples[i] * g)
        }
    }

    /** Transparent below threshold, then bends smoothly towards ceiling. */
    private fun limit(x: Double): Short {
        val a = abs(x)
        val y = if (a <= threshold) {
            a
        } else {
            threshold + (ceiling - threshold) * tanh((a - threshold) / (ceiling - threshold))
        }
        return (sign(x) * y).roundToInt().coerceIn(-32768, 32767).toShort()
    }

    private companion object {
        const val FULL_SCALE = 32767.0
    }
}
