/**
 * @file mp_net.h
 * @brief ENet networking wrapper for multiplayer
 *
 * Thin abstraction over ENet for host/client operations.
 * Host listens for connections and manages up to 3 client peers.
 * Client connects to a host and exchanges packets.
 */

#ifndef MP_NET_H
#define MP_NET_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque network context */
typedef struct MPNetContext MPNetContext;

/* ============================================================================
 * Initialization
 * ========================================================================== */

/** Initialize the networking subsystem (call once at startup) */
bool mp_net_init(void);

/** Shutdown the networking subsystem */
void mp_net_shutdown(void);

/* ============================================================================
 * Host API
 * ========================================================================== */

/** Create a host that listens on the given port. Returns NULL on failure. */
MPNetContext* mp_net_host_create(uint16_t port);

/** Accept pending connections and process incoming packets.
 *  Call once per frame. Returns number of events processed. */
int mp_net_host_service(MPNetContext* ctx, uint32_t timeout_ms);

/** Send a packet to a specific client (by slot index 0-2). */
bool mp_net_host_send(MPNetContext* ctx, int client_slot,
                      const void* data, uint32_t size, int channel);

/** Send a packet to all connected clients. */
void mp_net_host_broadcast(MPNetContext* ctx,
                           const void* data, uint32_t size, int channel);

/** Disconnect a specific client. */
void mp_net_host_kick(MPNetContext* ctx, int client_slot);

/** Get the number of currently connected clients. */
int mp_net_host_client_count(MPNetContext* ctx);

/** Get round-trip time (ping) for a client in ms. */
uint32_t mp_net_host_get_rtt(MPNetContext* ctx, int client_slot);

/* ============================================================================
 * Client API
 * ========================================================================== */

/** Create a client and connect to host_address:port. Returns NULL on failure. */
MPNetContext* mp_net_client_create(const char* host_address, uint16_t port);

/** Process incoming packets. Call once per frame. */
int mp_net_client_service(MPNetContext* ctx, uint32_t timeout_ms);

/** Send a packet to the host. */
bool mp_net_client_send(MPNetContext* ctx,
                        const void* data, uint32_t size, int channel);

/** Check if connected to host. */
bool mp_net_client_is_connected(MPNetContext* ctx);

/** Get round-trip time to host in ms. */
uint32_t mp_net_client_get_rtt(MPNetContext* ctx);

/** Disconnect from host. */
void mp_net_client_disconnect(MPNetContext* ctx);

/* ============================================================================
 * Common
 * ========================================================================== */

/** Destroy a network context (host or client). */
void mp_net_destroy(MPNetContext* ctx);

/* ============================================================================
 * Callbacks (set by mp_session to receive events)
 * ========================================================================== */

typedef void (*MPOnConnect)(int client_slot);
typedef void (*MPOnDisconnect)(int client_slot);
typedef void (*MPOnReceive)(int client_slot, const void* data, uint32_t size);

void mp_net_set_callbacks(MPNetContext* ctx,
                          MPOnConnect on_connect,
                          MPOnDisconnect on_disconnect,
                          MPOnReceive on_receive);

#ifdef __cplusplus
}
#endif

#endif /* MP_NET_H */
