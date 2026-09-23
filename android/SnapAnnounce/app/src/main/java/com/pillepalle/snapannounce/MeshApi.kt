package com.pillepalle.snapannounce

import android.net.Network
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets

/** One speaker in the server's device list, see GET /api/devices (webconfig.c). */
data class MeshDevice(
    /** Snapcast ID, the MAC for every real client; null for the server itself. */
    val id: String?,
    val name: String,
    /** Mesh hops from the server; 0 for the server, null when unknown. */
    val hops: Int?,
    /** One of ours: answers settings requests. False for a foreign Snapcast client. */
    val own: Boolean,
    /** Snapcast volume; for the server its volume knob, null without one. */
    val volumePercent: Int?,
    val muted: Boolean,
    /** Positive = later. For the server its knob or trim, read only here. */
    val delayMs: Int,
) {
    val isServer: Boolean get() = id == null
}

data class DeviceList(
    val serverId: String,
    val delayMaxMs: Int,
    /** Server first, then the clients by hops and name, as on the web page. */
    val devices: List<MeshDevice>,
)

/**
 * The server's HTTP API (port 80). Blocking calls, for Dispatchers.IO.
 *
 * The server's HTTP task handles one request at a time, and a request for
 * a client's settings can hold it for up to 3 s -- so callers should poll
 * gently and never in parallel with themselves.
 */
class MeshApi(private val host: String, private val network: Network?) {

    /** Null when the device at host is a client (it has no device list). */
    @Throws(IOException::class)
    fun devices(): DeviceList? {
        val root = JSONObject(request("GET", "/api/devices", null))
        val clients = root.optJSONArray("clients") ?: return null
        val server = root.getJSONObject("server")
        val serverId = server.getString("id")

        val list = mutableListOf(
            MeshDevice(
                id = null,
                name = "Dieser Server ($serverId)",
                hops = 0,
                own = true,
                volumePercent = server.optIntOrNull("volume_percent"),
                muted = false,
                delayMs = server.optInt("delay_ms"),
            )
        )
        val parsed = (0 until clients.length()).map { i ->
            val c = clients.getJSONObject(i)
            MeshDevice(
                id = c.getString("id"),
                name = c.getString("name"),
                hops = c.optIntOrNull("hops"),
                own = c.optBoolean("own"),
                volumePercent = c.optIntOrNull("volume_percent"),
                muted = c.optBoolean("muted"),
                delayMs = c.optInt("delay_ms"),
            )
        }
        list += parsed.sortedWith(compareBy<MeshDevice>({ it.hops ?: 99 }, { it.name }))
        return DeviceList(serverId, root.optInt("delay_max_ms", 2000), list)
    }

    /** Changes one client; fields left null keep their value. */
    @Throws(IOException::class)
    fun setDevice(
        id: String,
        volumePercent: Int? = null,
        muted: Boolean? = null,
        delayMs: Int? = null,
        name: String? = null,
    ) {
        val body = JSONObject().put("id", id)
        volumePercent?.let { body.put("volume_percent", it) }
        muted?.let { body.put("muted", it) }
        delayMs?.let { body.put("delay_ms", it) }
        name?.let { body.put("name", it) }
        request("POST", "/api/devices", body.toString())
    }

    /**
     * The config page for one device, as the app's settings view shows it:
     * embedded (no title, no device list of its own) and opened on that
     * device, or on the server itself when id is null.
     */
    fun settingsUrl(id: String?): String {
        val device = id?.let { "&device=" + java.net.URLEncoder.encode(it, "UTF-8") } ?: ""
        return "http://$host/?embed=1$device"
    }

    private fun request(method: String, path: String, body: String?): String {
        val url = URL("http://$host$path")
        val conn = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
        try {
            conn.requestMethod = method
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout = READ_TIMEOUT_MS
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.outputStream.use { it.write(body.toByteArray(StandardCharsets.UTF_8)) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.use { String(it.readBytes(), StandardCharsets.UTF_8) } ?: ""
            if (code !in 200..299) {
                throw IOException("HTTP $code: ${text.ifBlank { conn.responseMessage }}")
            }
            return text
        } finally {
            conn.disconnect()
        }
    }

    private fun JSONObject.optIntOrNull(key: String): Int? =
        if (has(key) && !isNull(key)) getInt(key) else null

    companion object {
        private const val CONNECT_TIMEOUT_MS = 3000
        /* Above the server's own 3 s wait on a client. */
        private const val READ_TIMEOUT_MS = 5000
    }
}
