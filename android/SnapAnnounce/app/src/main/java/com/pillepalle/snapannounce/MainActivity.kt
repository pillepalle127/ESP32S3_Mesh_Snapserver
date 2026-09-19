package com.pillepalle.snapannounce

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import android.media.MediaRecorder
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.ui.semantics.Role
import kotlin.math.roundToInt
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/**
 * One screen: server address, a latching button, and what the announcement
 * is doing right now.
 *
 * The button is a toggle, not push-to-talk: press once to go on air, press
 * again to stop. Going on air is not optimistic -- the UI only shows
 * "ON AIR" once the service has actually armed the server (Voice.Start
 * acknowledged), so a server that isn't reachable shows up as an error
 * instead of an open microphone talking to nobody.
 */
class MainActivity : ComponentActivity() {

    private val state = MutableStateFlow(UiState())
    private var collector: Job? = null
    private var bound = false

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val service = (binder as VoiceAnnounceService.LocalBinder).service()
            collector?.cancel()
            collector = lifecycleScope.launch {
                service.uiState.collect { state.value = it }
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            collector?.cancel()
            collector = null
        }
    }

    private val permissionRequest =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            // Start right away once the microphone was granted, instead of
            // making the user press the button a second time.
            if (granted[Manifest.permission.RECORD_AUDIO] == true) startAnnouncement()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val settings = SettingsStore(this)

        setContent {
            MaterialTheme {
                val uiState by state.collectAsStateWithLifecycle()
                AnnounceScreen(
                    uiState = uiState,
                    initialHost = settings.serverHost,
                    onHostChange = { settings.serverHost = it },
                    initialMicSource = settings.micSource,
                    onMicSourceChange = { settings.micSource = it },
                    initialMaxGainDb = settings.maxGainDb,
                    onMaxGainChange = { settings.maxGainDb = it },
                    onToggle = { armed ->
                        when {
                            armed -> stopAnnouncement()
                            hasMicPermission() -> startAnnouncement()
                            else -> permissionRequest.launch(requiredPermissions())
                        }
                    },
                )
            }
        }
    }

    override fun onStart() {
        super.onStart()
        // Bound (not started) while visible, only to observe state; the
        // service is *started* only for an actual announcement.
        bound = bindService(
            Intent(this, VoiceAnnounceService::class.java),
            connection,
            Context.BIND_AUTO_CREATE,
        )
    }

    override fun onStop() {
        collector?.cancel()
        collector = null
        if (bound) {
            unbindService(connection)
            bound = false
        }
        super.onStop()
    }

    private fun startAnnouncement() {
        val intent = Intent(this, VoiceAnnounceService::class.java)
            .setAction(VoiceAnnounceService.ACTION_START)
        startForegroundService(intent)
    }

    private fun stopAnnouncement() {
        val intent = Intent(this, VoiceAnnounceService::class.java)
            .setAction(VoiceAnnounceService.ACTION_STOP)
        startService(intent)
    }

    private fun hasMicPermission(): Boolean =
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Without it the foreground-service notification -- the only
            // visible sign of an open microphone once the app is in the
            // background -- would not be shown.
            arrayOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS)
        } else {
            arrayOf(Manifest.permission.RECORD_AUDIO)
        }
}

@Composable
private fun AnnounceScreen(
    uiState: UiState,
    initialHost: String,
    onHostChange: (String) -> Unit,
    initialMicSource: Int,
    onMicSourceChange: (Int) -> Unit,
    initialMaxGainDb: Int,
    onMaxGainChange: (Int) -> Unit,
    onToggle: (armed: Boolean) -> Unit,
) {
    var host by remember { mutableStateOf(initialHost) }
    var micSource by remember { mutableStateOf(initialMicSource) }
    var maxGainDb by remember { mutableStateOf(initialMaxGainDb) }
    val armed = uiState.state == AnnounceState.CONNECTING || uiState.state == AnnounceState.ON_AIR

    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Text("Durchsage", style = MaterialTheme.typography.headlineMedium)

            OutlinedTextField(
                value = host,
                onValueChange = {
                    host = it
                    onHostChange(it)
                },
                label = { Text("Server-IP") },
                singleLine = true,
                enabled = !armed,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth(),
            )

            StatusLine(uiState)

            Button(
                onClick = { onToggle(armed) },
                colors = if (armed) {
                    ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                } else {
                    ButtonDefaults.buttonColors()
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(120.dp),
            ) {
                Text(
                    text = if (armed) "Durchsage beenden" else "Durchsage starten",
                    style = MaterialTheme.typography.titleLarge,
                )
            }

            // Changes apply to the next announcement; locked while one runs.
            Text("Mikrofon", style = MaterialTheme.typography.titleMedium,
                 modifier = Modifier.fillMaxWidth())
            for ((source, label) in MIC_SOURCES) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier
                        .fillMaxWidth()
                        .selectable(
                            selected = micSource == source,
                            enabled = !armed,
                            role = Role.RadioButton,
                            onClick = {
                                micSource = source
                                onMicSourceChange(source)
                            },
                        ),
                ) {
                    RadioButton(selected = micSource == source, onClick = null, enabled = !armed)
                    Text(label, style = MaterialTheme.typography.bodyMedium,
                         modifier = Modifier.padding(start = 8.dp))
                }
            }

            Text(
                "Max. Verstärkung: $maxGainDb dB",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.fillMaxWidth(),
            )
            Slider(
                value = maxGainDb.toFloat(),
                onValueChange = { maxGainDb = (it / 3f).roundToInt() * 3 },
                onValueChangeFinished = { onMaxGainChange(maxGainDb) },
                valueRange = 0f..SettingsStore.MAX_GAIN_LIMIT_DB.toFloat(),
                steps = SettingsStore.MAX_GAIN_LIMIT_DB / 3 - 1,
                enabled = !armed,
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                "Mehr Verstärkung = lauter, aber auch mehr Hall und Rückkopplung. " +
                    "Handy nah an den Mund halten, von den Lautsprechern weg.",
                style = MaterialTheme.typography.bodySmall,
            )

            Text(
                "Das Handy muss mit dem Mesh-WLAN verbunden sein, möglichst direkt " +
                    "am Server. Wenn Android meldet, dass das WLAN keinen " +
                    "Internetzugang hat: trotzdem verbunden bleiben.",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

/** Selectable microphone sources, see SettingsStore.micSource. */
private val MIC_SOURCES = listOf(
    MediaRecorder.AudioSource.VOICE_COMMUNICATION to "Telefonat (Rauschunterdrückung)",
    MediaRecorder.AudioSource.MIC to "Standard-Mikrofon",
    MediaRecorder.AudioSource.VOICE_RECOGNITION to "Spracherkennung (unbearbeitet)",
    MediaRecorder.AudioSource.UNPROCESSED to "Roh (ohne jede Bearbeitung)",
)

@Composable
private fun StatusLine(uiState: UiState) {
    when (uiState.state) {
        AnnounceState.ON_AIR -> Text(
            text = "ON AIR  %d:%02d".format(uiState.elapsedSeconds / 60, uiState.elapsedSeconds % 60),
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.headlineSmall,
        )
        AnnounceState.CONNECTING -> Text("Verbinde…", style = MaterialTheme.typography.titleMedium)
        AnnounceState.ERROR -> Text(
            text = uiState.message ?: "Fehler",
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.titleMedium,
        )
        AnnounceState.IDLE -> Text(
            text = uiState.message ?: "Bereit",
            style = MaterialTheme.typography.titleMedium,
        )
    }
}
