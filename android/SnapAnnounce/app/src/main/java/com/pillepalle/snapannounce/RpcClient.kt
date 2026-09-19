package com.pillepalle.snapannounce

import android.net.Network
import org.json.JSONObject
import java.io.BufferedReader
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.charset.StandardCharsets

/**
 * Minimal JSON-RPC 2.0 client for the ESP32's control channel (port 1705,
 * snapcontrol.c): one JSON object per line, responses terminated with
 * CRLF. Only as much as Voice.Start/Voice.Stop need -- one blocking
 * request/response pair per call.
 *
 * The connection is kept open for the whole announcement on purpose: the
 * firmware stops an announcement when the connection that started it
 * closes (voice_announce_on_control_disconnect()), so the app being killed
 * mid-announcement ends it on the speakers too, without waiting for the
 * one-second silence watchdog.
 */
class RpcClient private constructor(private val socket: Socket) : AutoCloseable {

    private val reader =
        BufferedReader(InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8))
    private val output: OutputStream = socket.getOutputStream()
    private var nextId = 1

    /**
     * Sends one request and blocks for its response line. Returns the parsed
     * response object; callers check for an "error" member themselves, which
     * is how the firmware reports "another announcement is already active".
     */
    @Throws(IOException::class)
    fun call(method: String): JSONObject {
        val request = JSONObject()
            .put("jsonrpc", "2.0")
            .put("id", nextId++)
            .put("method", method)

        output.write((request.toString() + "\r\n").toByteArray(StandardCharsets.UTF_8))
        output.flush()

        // The server also pushes unsolicited Server.OnUpdate notifications
        // on this connection whenever the client set changes (see
        // ctrl_conn_task in snapcontrol.c) -- skip anything that isn't a
        // response, i.e. has no "id".
        while (true) {
            val line = reader.readLine()
                ?: throw IOException("Connection closed while waiting for $method")
            if (line.isBlank()) continue
            val obj = JSONObject(line)
            if (obj.has("id") && !obj.isNull("id")) return obj
        }
    }

    override fun close() {
        try {
            socket.close()
        } catch (_: IOException) {
            // already gone, nothing to do
        }
    }

    companion object {
        /**
         * Blocking connect with an explicit timeout, bound to the given
         * network when one is passed -- the mesh has no internet uplink, so
         * letting Android route this over mobile data would just time out
         * against an address nobody on the internet has.
         */
        @Throws(IOException::class)
        fun connect(
            host: String,
            port: Int,
            connectTimeoutMs: Int,
            readTimeoutMs: Int,
            network: Network?,
        ): RpcClient {
            val socket = Socket()
            try {
                network?.bindSocket(socket)
                socket.connect(InetSocketAddress(host, port), connectTimeoutMs)
                socket.soTimeout = readTimeoutMs
                socket.tcpNoDelay = true
            } catch (e: IOException) {
                try {
                    socket.close()
                } catch (_: IOException) {
                }
                throw e
            }
            return RpcClient(socket)
        }
    }
}
