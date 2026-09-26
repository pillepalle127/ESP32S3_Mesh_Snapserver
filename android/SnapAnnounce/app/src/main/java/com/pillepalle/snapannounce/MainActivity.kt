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
import androidx.compose.foundation.layout.Box
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.ui.semantics.Role
import kotlin.math.roundToInt
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import androidx.compose.ui.res.stringResource

/**
 * Two tabs: the announcement (server address, a latching button, and what
 * the announcement is doing right now) and the server's device list, from
 * which a device's full settings open (DevicesScreen.kt).
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
                var tab by rememberSaveable { mutableIntStateOf(TAB_ANNOUNCE) }
                // Non-null while one device's settings are open over everything else.
                var settingsFor by remember { mutableStateOf<MeshDevice?>(null) }

                val open = settingsFor
                if (open != null) {
                    DeviceSettingsScreen(
                        url = MeshApi(settings.serverHost, null).settingsUrl(open.id),
                        title = open.name,
                        onBack = { settingsFor = null },
                    )
                } else {
                    Scaffold(
                        bottomBar = {
                            NavigationBar {
                                NavigationBarItem(
                                    selected = tab == TAB_ANNOUNCE,
                                    onClick = { tab = TAB_ANNOUNCE },
                                    icon = { Icon(Icons.Filled.Phone, contentDescription = null) },
                                    label = { Text(stringResource(R.string.tab_announce)) },
                                )
                                NavigationBarItem(
                                    selected = tab == TAB_DEVICES,
                                    onClick = { tab = TAB_DEVICES },
                                    icon = { Icon(Icons.AutoMirrored.Filled.List, contentDescription = null) },
                                    label = { Text(stringResource(R.string.tab_devices)) },
                                )
                            }
                        },
                    ) { padding ->
                        Box(modifier = Modifier.padding(padding)) {
                            if (tab == TAB_DEVICES) {
                                DevicesScreen(
                                    host = settings.serverHost,
                                    onOpenSettings = { settingsFor = it },
                                )
                            } else {
                                AnnounceScreen(
                                    uiState = uiState,
                                    initialHost = settings.serverHost,
                                    onHostChange = { settings.serverHost = it },
                                    initialMicSource = settings.micSource,
                                    onMicSourceChange = { settings.micSource = it },
                                    initialMaxGainDb = settings.maxGainDb,
                                    onMaxGainChange = { settings.maxGainDb = it },
                                    initialTargetRmsDbfs = settings.targetRmsDbfs,
                                    onTargetRmsChange = { settings.targetRmsDbfs = it },
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
                }
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
    initialTargetRmsDbfs: Int,
    onTargetRmsChange: (Int) -> Unit,
    onToggle: (armed: Boolean) -> Unit,
) {
    var host by remember { mutableStateOf(initialHost) }
    var micSource by remember { mutableStateOf(initialMicSource) }
    var maxGainDb by remember { mutableStateOf(initialMaxGainDb) }
    var targetRmsDbfs by remember { mutableStateOf(initialTargetRmsDbfs) }
    /* Everything that is set once and then left alone lives behind this,
     * so the screen one actually uses is the button and nothing else. */
    var showSettings by remember { mutableStateOf(false) }
    val armed = uiState.state == AnnounceState.CONNECTING || uiState.state == AnnounceState.ON_AIR

    Box(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Text(stringResource(R.string.tab_announce), style = MaterialTheme.typography.headlineMedium)

            OutlinedTextField(
                value = host,
                onValueChange = {
                    host = it
                    onHostChange(it)
                },
                label = { Text(stringResource(R.string.server_ip)) },
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
                    .height(160.dp),
            ) {
                /* The logo carries the button; the label only says what a
                 * press does now. Tinted to the button's content colour so
                 * the black artwork stays readable on both the idle and the
                 * red "on air" background. */
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Image(
                        painter = painterResource(R.drawable.cm_logo),
                        contentDescription = null,
                        colorFilter = ColorFilter.tint(
                            if (armed) {
                                MaterialTheme.colorScheme.onError
                            } else {
                                MaterialTheme.colorScheme.onPrimary
                            }
                        ),
                        modifier = Modifier.height(84.dp),
                    )
                    Text(
                        text = stringResource(if (armed) R.string.announce_stop else R.string.announce_start),
                        style = MaterialTheme.typography.titleLarge,
                    )
                }
            }

            TextButton(onClick = { showSettings = true }, enabled = !armed) {
                Text(stringResource(R.string.settings))
            }
        }

        if (showSettings) {
            SettingsDialog(
                micSource = micSource,
                onMicSourceChange = {
                    micSource = it
                    onMicSourceChange(it)
                },
                maxGainDb = maxGainDb,
                onMaxGainChange = {
                    maxGainDb = it
                    onMaxGainChange(it)
                },
                targetRmsDbfs = targetRmsDbfs,
                onTargetRmsChange = {
                    targetRmsDbfs = it
                    onTargetRmsChange(it)
                },
                onDismiss = { showSettings = false },
            )
        }
    }
}

private const val TAB_ANNOUNCE = 0
private const val TAB_DEVICES = 1

/** Selectable microphone sources, see SettingsStore.micSource. */
private val MIC_SOURCES = listOf(
    MediaRecorder.AudioSource.MIC to R.string.mic_default,
    MediaRecorder.AudioSource.UNPROCESSED to R.string.mic_unprocessed,
    MediaRecorder.AudioSource.VOICE_RECOGNITION to R.string.mic_voice_recognition,
    MediaRecorder.AudioSource.VOICE_COMMUNICATION to R.string.mic_voice_communication,
)

/*
 * Set once for a given phone and room, then left alone -- so it lives here
 * rather than on the screen one reaches for to make an announcement.
 */
@Composable
private fun SettingsDialog(
    micSource: Int,
    onMicSourceChange: (Int) -> Unit,
    maxGainDb: Int,
    onMaxGainChange: (Int) -> Unit,
    targetRmsDbfs: Int,
    onTargetRmsChange: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    var gain by remember { mutableStateOf(maxGainDb) }
    var target by remember { mutableStateOf(targetRmsDbfs) }

    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.done)) } },
        title = { Text(stringResource(R.string.settings)) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(stringResource(R.string.microphone), style = MaterialTheme.typography.titleMedium)
                for ((source, label) in MIC_SOURCES) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .fillMaxWidth()
                            .selectable(
                                selected = micSource == source,
                                role = Role.RadioButton,
                                onClick = { onMicSourceChange(source) },
                            ),
                    ) {
                        RadioButton(selected = micSource == source, onClick = null)
                        Text(stringResource(label), style = MaterialTheme.typography.bodyMedium,
                             modifier = Modifier.padding(start = 8.dp))
                    }
                }

                Text(stringResource(R.string.max_gain, gain),
                     style = MaterialTheme.typography.titleMedium)
                Slider(
                    value = gain.toFloat(),
                    onValueChange = { gain = (it / 3f).roundToInt() * 3 },
                    onValueChangeFinished = { onMaxGainChange(gain) },
                    valueRange = 0f..SettingsStore.MAX_GAIN_LIMIT_DB.toFloat(),
                    steps = SettingsStore.MAX_GAIN_LIMIT_DB / 3 - 1,
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    stringResource(R.string.max_gain_hint),
                    style = MaterialTheme.typography.bodySmall,
                )

                Text(stringResource(R.string.announce_level, target),
                     style = MaterialTheme.typography.titleMedium)
                Slider(
                    value = target.toFloat(),
                    onValueChange = { target = (it / 2f).roundToInt() * 2 },
                    onValueChangeFinished = { onTargetRmsChange(target) },
                    valueRange = SettingsStore.MIN_TARGET_RMS_DBFS.toFloat()..
                        SettingsStore.MAX_TARGET_RMS_DBFS.toFloat(),
                    steps = (SettingsStore.MAX_TARGET_RMS_DBFS -
                        SettingsStore.MIN_TARGET_RMS_DBFS) / 2 - 1,
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    stringResource(R.string.announce_level_hint),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        },
    )
}

@Composable
private fun StatusLine(uiState: UiState) {
    when (uiState.state) {
        AnnounceState.ON_AIR -> Text(
            text = "ON AIR  %d:%02d".format(uiState.elapsedSeconds / 60, uiState.elapsedSeconds % 60),
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.headlineSmall,
        )
        AnnounceState.CONNECTING -> Text(stringResource(R.string.connecting), style = MaterialTheme.typography.titleMedium)
        AnnounceState.ERROR -> Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = uiState.message ?: stringResource(R.string.error),
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.titleMedium,
            )
            // Only when it matters: the usual cause is a phone that is not
            // (or no longer) on the mesh Wi-Fi.
            if (uiState.serverNotFound) {
                Text(
                    stringResource(R.string.hint_mesh_wifi),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
        AnnounceState.IDLE -> Text(
            text = uiState.message ?: stringResource(R.string.ready),
            style = MaterialTheme.typography.titleMedium,
        )
    }
}
