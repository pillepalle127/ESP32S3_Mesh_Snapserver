package com.pillepalle.snapannounce

import android.annotation.SuppressLint
import android.net.ConnectivityManager
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/* The web page polls every 3 s as well; the server answers one request at a time. */
private const val POLL_MS = 3000L
/* Delay taps are collected this long and sent as one change: a client
 * resyncs hard on a jump over 100 ms, so ten single +10 steps would each
 * be a small correction instead of one. */
private const val DELAY_SEND_AFTER_MS = 700L

/**
 * The server's device list, natively: what is used day to day -- volume,
 * mute, delay, names. A device's full settings open in the firmware's own
 * page (DeviceSettingsScreen), so that form and its rules live in one place.
 */
@Composable
fun DevicesScreen(host: String, onOpenSettings: (MeshDevice) -> Unit) {
    val context = LocalContext.current
    val connectivity = remember { context.getSystemService(ConnectivityManager::class.java) }
    val scope = rememberCoroutineScope()

    var list by remember { mutableStateOf<DeviceList?>(null) }
    var isClient by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var refresh by remember { mutableIntStateOf(0) }
    var renaming by remember { mutableStateOf<MeshDevice?>(null) }

    fun api() = MeshApi(host, wifiNetworkOrNull(connectivity))

    /* Applies a change locally right away, so the next poll does not flip
     * the control back while the request is still under way. */
    fun change(dev: MeshDevice, updated: MeshDevice, send: MeshApi.() -> Unit) {
        list = list?.let { l -> l.copy(devices = l.devices.map { if (it.id == dev.id) updated else it }) }
        scope.launch {
            try {
                withContext(Dispatchers.IO) { api().send() }
                error = null
            } catch (e: Exception) {
                error = "Nicht übernommen: ${e.message}"
                refresh++
            }
        }
    }

    // Runs while this screen is shown, restarts on refresh.
    LaunchedEffect(host, refresh) {
        while (true) {
            try {
                val result = withContext(Dispatchers.IO) { api().devices() }
                isClient = result == null
                list = result
                error = null
            } catch (e: Exception) {
                error = "Server nicht erreichbar: ${e.message}"
            }
            delay(POLL_MS)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp),
    ) {
        Text(
            "Geräte",
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.padding(vertical = 16.dp),
        )
        error?.let {
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium)
        }
        if (isClient) {
            Text(
                "$host ist ein Client. Die Geräteliste gibt es nur auf dem Server; " +
                    "unter Durchsage die Server-IP eintragen.",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(vertical = 8.dp),
            )
            OutlinedButton(onClick = {
                onOpenSettings(MeshDevice(null, host, null, true, null, false, 0))
            }) { Text("Einstellungen dieses Geräts") }
        }

        val current = list
        if (current != null) {
            LazyColumn(
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxSize(),
            ) {
                // Keys must be unique; an old firmware can list a reconnected
                // client twice until the stale connection times out.
                items(current.devices.distinctBy { it.id }, key = { it.id ?: "\u0000server" }) { dev ->
                    DeviceCard(
                        dev = dev,
                        delayMaxMs = current.delayMaxMs,
                        onVolume = { v ->
                            change(dev, dev.copy(volumePercent = v)) { setDevice(dev.id!!, volumePercent = v) }
                        },
                        onMute = { m ->
                            change(dev, dev.copy(muted = m)) { setDevice(dev.id!!, muted = m) }
                        },
                        onDelay = { d ->
                            change(dev, dev.copy(delayMs = d)) { setDevice(dev.id!!, delayMs = d) }
                        },
                        onRename = { renaming = dev },
                        onOpenSettings = { onOpenSettings(dev) },
                    )
                }
            }
        } else if (error == null && !isClient) {
            Text("Lade…", style = MaterialTheme.typography.bodyMedium)
        }
    }

    renaming?.let { dev ->
        RenameDialog(
            initial = dev.name,
            onDismiss = { renaming = null },
            onConfirm = { name ->
                renaming = null
                change(dev, dev.copy(name = name)) { setDevice(dev.id!!, name = name) }
            },
        )
    }
}

@Composable
private fun DeviceCard(
    dev: MeshDevice,
    delayMaxMs: Int,
    onVolume: (Int) -> Unit,
    onMute: (Boolean) -> Unit,
    onDelay: (Int) -> Unit,
    onRename: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    dev.name,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier
                        .weight(1f)
                        .then(if (dev.isServer) Modifier else Modifier.clickable(onClick = onRename)),
                )
                Text(hopsLabel(dev.hops), style = MaterialTheme.typography.labelMedium)
            }
            if (!dev.own) {
                AssistChip(onClick = {}, enabled = false, label = { Text("Snapcast") })
            }

            if (dev.isServer) {
                // Knob and trim of the server are set on the device or in its settings.
                Text(
                    "Lautstärke-Poti: " + (dev.volumePercent?.let { "$it %" } ?: "keins") +
                        "   Delay: ${signed(dev.delayMs)} ms",
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else {
                VolumeRow(dev, onVolume, onMute)
                DelayRow(dev.id!!, dev.delayMs, delayMaxMs, onDelay)
            }

            if (dev.own) {
                TextButton(onClick = onOpenSettings) { Text("Einstellungen") }
            }
        }
    }
}

@Composable
private fun VolumeRow(dev: MeshDevice, onVolume: (Int) -> Unit, onMute: (Boolean) -> Unit) {
    // Only while the thumb is held; afterwards the list value counts again.
    var dragging by remember(dev.id) { mutableStateOf<Float?>(null) }
    val shown = dragging ?: (dev.volumePercent ?: 100).toFloat()
    Row(verticalAlignment = Alignment.CenterVertically) {
        Slider(
            value = shown,
            onValueChange = { dragging = it },
            onValueChangeFinished = {
                dragging?.let { onVolume(it.roundToInt()) }
                dragging = null
            },
            valueRange = 0f..100f,
            modifier = Modifier.weight(1f),
        )
        Text("${shown.roundToInt()} %", maxLines = 1, modifier = Modifier.width(60.dp))
        Switch(checked = dev.muted, onCheckedChange = onMute)
        Text("stumm", style = MaterialTheme.typography.labelSmall, modifier = Modifier.padding(start = 4.dp))
    }
}

/*
 * Buttons instead of a number field: the delay can be negative, and many
 * Android number keyboards have no minus key.
 */
@Composable
private fun DelayRow(id: String, delayMs: Int, delayMaxMs: Int, onDelay: (Int) -> Unit) {
    var pending by remember(id) { mutableStateOf<Int?>(null) }
    val shown = pending ?: delayMs

    LaunchedEffect(pending) {
        val value = pending ?: return@LaunchedEffect
        delay(DELAY_SEND_AFTER_MS)
        onDelay(value)
        pending = null
    }

    fun step(by: Int) {
        pending = (shown + by).coerceIn(-delayMaxMs, delayMaxMs)
    }

    Text("Delay: ${signed(shown)} ms", style = MaterialTheme.typography.bodyMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        for (by in listOf(-100, -10, 10, 100)) {
            OutlinedButton(
                onClick = { step(by) },
                contentPadding = PaddingValues(0.dp),
                modifier = Modifier.weight(1f),
            ) { Text(if (by < 0) "−${-by}" else "+$by") }
        }
    }
}

@Composable
private fun RenameDialog(initial: String, onDismiss: () -> Unit, onConfirm: (String) -> Unit) {
    var name by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Umbenennen") },
        text = {
            OutlinedTextField(value = name, onValueChange = { name = it.take(63) }, singleLine = true)
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(name.trim()) }, enabled = name.isNotBlank()) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Abbrechen") } },
    )
}

/**
 * One device's full settings: the firmware's config page, embedded and
 * opened on that device (see MeshApi.settingsUrl()).
 *
 * The WebView cannot be bound to a network on its own, so the whole
 * process is bound to the mesh Wi-Fi while this is open -- otherwise
 * Android would send the page's requests over mobile data. The announcement
 * service binds its own sockets explicitly and is not affected.
 */
@OptIn(ExperimentalMaterial3Api::class)
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun DeviceSettingsScreen(url: String, title: String, onBack: () -> Unit) {
    val context = LocalContext.current
    val connectivity = remember { context.getSystemService(ConnectivityManager::class.java) }
    val webView = remember {
        WebView(context).apply {
            settings.javaScriptEnabled = true
            webViewClient = WebViewClient()
            // Default dialogs, e.g. the page's factory-reset confirm().
            webChromeClient = WebChromeClient()
        }
    }

    DisposableEffect(url) {
        val network = wifiNetworkOrNull(connectivity)
        val bound = network != null && connectivity.bindProcessToNetwork(network)
        webView.loadUrl(url)
        onDispose {
            if (bound) connectivity.bindProcessToNetwork(null)
            webView.destroy()
        }
    }
    BackHandler(onBack = onBack)

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(title) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Zurück")
                    }
                },
            )
        },
    ) { padding ->
        AndroidView(
            factory = { webView },
            modifier = Modifier
                .padding(padding)
                .fillMaxSize(),
        )
    }
}

private fun hopsLabel(hops: Int?): String = when (hops) {
    null -> "Hops ?"
    0 -> "Server"
    1 -> "1 Hop"
    else -> "$hops Hops"
}

private fun signed(ms: Int): String = if (ms > 0) "+$ms" else "$ms"
