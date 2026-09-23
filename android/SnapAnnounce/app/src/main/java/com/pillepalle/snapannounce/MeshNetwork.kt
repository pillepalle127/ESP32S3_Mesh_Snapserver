package com.pillepalle.snapannounce

import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities

/**
 * The Wi-Fi network the mesh is on, or null if the phone has none.
 *
 * Everything that talks to the ESP32 is bound to this: the mesh has no
 * internet uplink, so Android prefers mobile data as the default network
 * and would route a request for 192.168.5.1 there, where it times out.
 *
 * The active network is the cheap answer when it already is Wi-Fi. Past
 * that there is no non-deprecated way to list networks synchronously --
 * the replacement is a callback that only reports later -- and callers
 * need the answer now, right before they open a socket.
 */
fun wifiNetworkOrNull(connectivity: ConnectivityManager): Network? {
    connectivity.activeNetwork?.let { active ->
        if (connectivity.getNetworkCapabilities(active)
                ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        ) {
            return active
        }
    }
    @Suppress("DEPRECATION")
    return connectivity.allNetworks.firstOrNull { net ->
        connectivity.getNetworkCapabilities(net)
            ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
    }
}
