/**
 * @file mp_net.cpp
 * @brief ENet networking implementation
 */

#include "mp_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <enet/enet.h>

/* ============================================================================
 * Internal Types
 * ========================================================================== */

struct MPNetContext {
    bool is_host;
    ENetHost* host;

    /* Host: connected client peers (up to MP_MAX_PLAYERS - 1) */
    ENetPeer* clients[MP_MAX_PLAYERS - 1];
    int client_count;

    /* Client: connection to host */
    ENetPeer* server_peer;
    bool connected;

    /* Callbacks */
    MPOnConnect    on_connect;
    MPOnDisconnect on_disconnect;
    MPOnReceive    on_receive;
};

/* ============================================================================
 * Init / Shutdown
 * ========================================================================== */

static bool g_enet_initialized = false;

bool mp_net_init(void) {
    if (g_enet_initialized) return true;

    if (enet_initialize() != 0) {
        fprintf(stderr, "[MP_NET] ENet initialization failed\n");
        return false;
    }

    g_enet_initialized = true;
    fprintf(stderr, "[MP_NET] ENet initialized\n");
    return true;
}

void mp_net_shutdown(void) {
    if (g_enet_initialized) {
        enet_deinitialize();
        g_enet_initialized = false;
        fprintf(stderr, "[MP_NET] ENet shut down\n");
    }
}

/* ============================================================================
 * Host API
 * ========================================================================== */

MPNetContext* mp_net_host_create(uint16_t port) {
    if (!g_enet_initialized && !mp_net_init()) return NULL;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    /* Allow up to 3 clients (host is player 0) */
    ENetHost* host = enet_host_create(&address,
                                       MP_MAX_PLAYERS - 1,
                                       MP_CHANNEL_COUNT,
                                       0, /* unlimited incoming bandwidth */
                                       0  /* unlimited outgoing bandwidth */);
    if (!host) {
        fprintf(stderr, "[MP_NET] Failed to create host on port %u\n", port);
        return NULL;
    }

    MPNetContext* ctx = (MPNetContext*)calloc(1, sizeof(MPNetContext));
    ctx->is_host = true;
    ctx->host = host;
    ctx->client_count = 0;

    fprintf(stderr, "[MP_NET] Host created on port %u\n", port);
    return ctx;
}

int mp_net_host_service(MPNetContext* ctx, uint32_t timeout_ms) {
    if (!ctx || !ctx->is_host) return 0;

    ENetEvent event;
    int events_processed = 0;

    while (enet_host_service(ctx->host, &event, timeout_ms) > 0) {
        timeout_ms = 0; /* Only wait on the first call */
        events_processed++;

        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            /* Find an empty slot */
            int slot = -1;
            for (int i = 0; i < MP_MAX_PLAYERS - 1; i++) {
                if (!ctx->clients[i]) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                /* No room - disconnect */
                fprintf(stderr, "[MP_NET] Client rejected (server full)\n");
                enet_peer_disconnect(event.peer, 0);
                break;
            }

            ctx->clients[slot] = event.peer;
            event.peer->data = (void*)(intptr_t)slot;
            ctx->client_count++;

            fprintf(stderr, "[MP_NET] Client connected -> slot %d (%d total)\n",
                    slot, ctx->client_count);

            if (ctx->on_connect) ctx->on_connect(slot);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            int slot = (int)(intptr_t)event.peer->data;
            if (slot >= 0 && slot < MP_MAX_PLAYERS - 1) {
                ctx->clients[slot] = NULL;
                ctx->client_count--;
                fprintf(stderr, "[MP_NET] Client disconnected from slot %d\n", slot);
                if (ctx->on_disconnect) ctx->on_disconnect(slot);
            }
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            int slot = (int)(intptr_t)event.peer->data;
            if (ctx->on_receive)
                ctx->on_receive(slot, event.packet->data, (uint32_t)event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        }

        default:
            break;
        }
    }

    return events_processed;
}

bool mp_net_host_send(MPNetContext* ctx, int client_slot,
                      const void* data, uint32_t size, int channel)
{
    if (!ctx || !ctx->is_host) return false;
    if (client_slot < 0 || client_slot >= MP_MAX_PLAYERS - 1) return false;
    if (!ctx->clients[client_slot]) return false;

    uint32_t flags = (channel == MP_CHANNEL_RELIABLE)
                     ? ENET_PACKET_FLAG_RELIABLE
                     : ENET_PACKET_FLAG_UNSEQUENCED;

    ENetPacket* packet = enet_packet_create(data, size, flags);
    return enet_peer_send(ctx->clients[client_slot], channel, packet) == 0;
}

void mp_net_host_broadcast(MPNetContext* ctx,
                           const void* data, uint32_t size, int channel)
{
    if (!ctx || !ctx->is_host) return;

    for (int i = 0; i < MP_MAX_PLAYERS - 1; i++) {
        if (ctx->clients[i]) {
            mp_net_host_send(ctx, i, data, size, channel);
        }
    }
}

void mp_net_host_kick(MPNetContext* ctx, int client_slot) {
    if (!ctx || !ctx->is_host) return;
    if (client_slot < 0 || client_slot >= MP_MAX_PLAYERS - 1) return;
    if (ctx->clients[client_slot]) {
        enet_peer_disconnect(ctx->clients[client_slot], 0);
    }
}

int mp_net_host_client_count(MPNetContext* ctx) {
    return ctx ? ctx->client_count : 0;
}

uint32_t mp_net_host_get_rtt(MPNetContext* ctx, int client_slot) {
    if (!ctx || !ctx->is_host) return 0;
    if (client_slot < 0 || client_slot >= MP_MAX_PLAYERS - 1) return 0;
    if (!ctx->clients[client_slot]) return 0;
    return ctx->clients[client_slot]->roundTripTime;
}

/* ============================================================================
 * Client API
 * ========================================================================== */

MPNetContext* mp_net_client_create(const char* host_address, uint16_t port) {
    if (!g_enet_initialized && !mp_net_init()) return NULL;

    ENetHost* client = enet_host_create(NULL, /* client, no binding */
                                         1,   /* one connection (to server) */
                                         MP_CHANNEL_COUNT,
                                         0, 0);
    if (!client) {
        fprintf(stderr, "[MP_NET] Failed to create client host\n");
        return NULL;
    }

    ENetAddress address;
    enet_address_set_host(&address, host_address);
    address.port = port;

    ENetPeer* peer = enet_host_connect(client, &address, MP_CHANNEL_COUNT, 0);
    if (!peer) {
        fprintf(stderr, "[MP_NET] Failed to initiate connection to %s:%u\n",
                host_address, port);
        enet_host_destroy(client);
        return NULL;
    }

    MPNetContext* ctx = (MPNetContext*)calloc(1, sizeof(MPNetContext));
    ctx->is_host = false;
    ctx->host = client;
    ctx->server_peer = peer;
    ctx->connected = false;

    fprintf(stderr, "[MP_NET] Connecting to %s:%u...\n", host_address, port);
    return ctx;
}

int mp_net_client_service(MPNetContext* ctx, uint32_t timeout_ms) {
    if (!ctx || ctx->is_host) return 0;

    ENetEvent event;
    int events_processed = 0;

    while (enet_host_service(ctx->host, &event, timeout_ms) > 0) {
        timeout_ms = 0;
        events_processed++;

        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            ctx->connected = true;
            fprintf(stderr, "[MP_NET] Connected to host!\n");
            if (ctx->on_connect) ctx->on_connect(0);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            ctx->connected = false;
            ctx->server_peer = NULL;
            fprintf(stderr, "[MP_NET] Disconnected from host\n");
            if (ctx->on_disconnect) ctx->on_disconnect(0);
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            if (ctx->on_receive)
                ctx->on_receive(0, event.packet->data, (uint32_t)event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;

        default:
            break;
        }
    }

    return events_processed;
}

bool mp_net_client_send(MPNetContext* ctx,
                        const void* data, uint32_t size, int channel)
{
    if (!ctx || ctx->is_host || !ctx->connected || !ctx->server_peer)
        return false;

    uint32_t flags = (channel == MP_CHANNEL_RELIABLE)
                     ? ENET_PACKET_FLAG_RELIABLE
                     : ENET_PACKET_FLAG_UNSEQUENCED;

    ENetPacket* packet = enet_packet_create(data, size, flags);
    return enet_peer_send(ctx->server_peer, channel, packet) == 0;
}

bool mp_net_client_is_connected(MPNetContext* ctx) {
    return ctx && !ctx->is_host && ctx->connected;
}

uint32_t mp_net_client_get_rtt(MPNetContext* ctx) {
    if (!ctx || ctx->is_host || !ctx->server_peer) return 0;
    return ctx->server_peer->roundTripTime;
}

void mp_net_client_disconnect(MPNetContext* ctx) {
    if (!ctx || ctx->is_host || !ctx->server_peer) return;
    enet_peer_disconnect(ctx->server_peer, 0);
}

/* ============================================================================
 * Common
 * ========================================================================== */

void mp_net_destroy(MPNetContext* ctx) {
    if (!ctx) return;

    if (ctx->is_host) {
        /* Kick all clients first */
        for (int i = 0; i < MP_MAX_PLAYERS - 1; i++) {
            if (ctx->clients[i])
                enet_peer_disconnect_now(ctx->clients[i], 0);
        }
    } else {
        if (ctx->server_peer)
            enet_peer_disconnect_now(ctx->server_peer, 0);
    }

    if (ctx->host)
        enet_host_destroy(ctx->host);

    free(ctx);
}

void mp_net_set_callbacks(MPNetContext* ctx,
                          MPOnConnect on_connect,
                          MPOnDisconnect on_disconnect,
                          MPOnReceive on_receive)
{
    if (!ctx) return;
    ctx->on_connect = on_connect;
    ctx->on_disconnect = on_disconnect;
    ctx->on_receive = on_receive;
}
