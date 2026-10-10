/**
 * @file wled_proxy.h
 * @brief Server only: takes TCP connections on a WLED's port and passes
 *        them on to where the WLED is, see wled_proxy.c.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connections to `port` on the server go to target_ip:target_port (lwIP
 * byte order for the address). Lapses 90 s after the last call, so the
 * caller renews it while the WLED is there. Starts the proxy task on the
 * first call.
 */
void wled_proxy_set(uint16_t port, uint32_t target_ip, uint16_t target_port);

#ifdef __cplusplus
}
#endif
