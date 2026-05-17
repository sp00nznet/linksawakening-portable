/**
 * @file mp_trade.cpp
 * @brief Item trading implementation
 */

#include "mp_trade.h"
#include "mp_session.h"
#include "mp_net.h"
#include <string.h>
#include <stdio.h>

extern "C" {
#include "gbrt.h"
}

/* WRAM addresses for tradeable quantities */
#define WRAM_RUPEES_HIGH   0xDB5D
#define WRAM_RUPEES_LOW    0xDB5E
#define WRAM_BOMBS         0xDB4D
#define WRAM_ARROWS        0xDB45
#define WRAM_POWDER        0xDB4C
#define WRAM_HEALTH        0xDB5A
#define WRAM_MAX_HEALTH    0xDB5B
#define WRAM_SEASHELLS     0xDBBB
#define WRAM_SMALL_KEYS    0xDC78

/* ============================================================================
 * State
 * ========================================================================== */

static struct {
    MPTradeState state;
    uint8_t      pending_from;
    uint8_t      pending_to;
    uint8_t      pending_item;
    float        status_timer;
    char         status_msg[64];
} g_trade = { .state = TRADE_STATE_NONE };

/* ============================================================================
 * WRAM Helpers
 * ========================================================================== */

static uint8_t read_w(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram || addr < 0xC000 || addr >= 0xE000) return 0;
    return ctx->wram[addr - 0xC000];
}

static void write_w(GBContext* ctx, uint16_t addr, uint8_t val) {
    if (!ctx || !ctx->wram || addr < 0xC000 || addr >= 0xE000) return;
    ctx->wram[addr - 0xC000] = val;
}

static int get_rupees(GBContext* ctx) {
    return (read_w(ctx, WRAM_RUPEES_HIGH) << 8) | read_w(ctx, WRAM_RUPEES_LOW);
}

static void set_rupees(GBContext* ctx, int amount) {
    if (amount < 0) amount = 0;
    if (amount > 999) amount = 999;
    write_w(ctx, WRAM_RUPEES_HIGH, (amount >> 8) & 0xFF);
    write_w(ctx, WRAM_RUPEES_LOW, amount & 0xFF);
}

/* ============================================================================
 * Trade Validation & Execution
 * ========================================================================== */

static bool can_afford(GBContext* ctx, MPTradeItemType item) {
    switch (item) {
    case TRADE_ITEM_RUPEES_50:    return get_rupees(ctx) >= 50;
    case TRADE_ITEM_RUPEES_100:   return get_rupees(ctx) >= 100;
    case TRADE_ITEM_RUPEES_200:   return get_rupees(ctx) >= 200;
    case TRADE_ITEM_BOMBS_5:      return read_w(ctx, WRAM_BOMBS) >= 5;
    case TRADE_ITEM_BOMBS_10:     return read_w(ctx, WRAM_BOMBS) >= 10;
    case TRADE_ITEM_ARROWS_5:     return read_w(ctx, WRAM_ARROWS) >= 5;
    case TRADE_ITEM_ARROWS_10:    return read_w(ctx, WRAM_ARROWS) >= 10;
    case TRADE_ITEM_POWDER_5:     return read_w(ctx, WRAM_POWDER) >= 5;
    case TRADE_ITEM_HEART:        return read_w(ctx, WRAM_HEALTH) > 8;
    case TRADE_ITEM_SEASHELL:     return read_w(ctx, WRAM_SEASHELLS) >= 1;
    case TRADE_ITEM_KEY:          return read_w(ctx, WRAM_SMALL_KEYS) >= 1;
    default: return false;
    }
}

void mp_trade_execute(void* ctx_from_ptr, void* ctx_to_ptr, MPTradeItemType item) {
    GBContext* from = (GBContext*)ctx_from_ptr;
    GBContext* to   = (GBContext*)ctx_to_ptr;
    if (!from || !to) return;

    if (!can_afford(from, item)) {
        fprintf(stderr, "[TRADE] Sender can't afford item %d\n", item);
        return;
    }

    switch (item) {
    case TRADE_ITEM_RUPEES_50:
        set_rupees(from, get_rupees(from) - 50);
        set_rupees(to, get_rupees(to) + 50);
        break;
    case TRADE_ITEM_RUPEES_100:
        set_rupees(from, get_rupees(from) - 100);
        set_rupees(to, get_rupees(to) + 100);
        break;
    case TRADE_ITEM_RUPEES_200:
        set_rupees(from, get_rupees(from) - 200);
        set_rupees(to, get_rupees(to) + 200);
        break;
    case TRADE_ITEM_BOMBS_5:
        write_w(from, WRAM_BOMBS, read_w(from, WRAM_BOMBS) - 5);
        write_w(to, WRAM_BOMBS, read_w(to, WRAM_BOMBS) + 5);
        break;
    case TRADE_ITEM_BOMBS_10:
        write_w(from, WRAM_BOMBS, read_w(from, WRAM_BOMBS) - 10);
        write_w(to, WRAM_BOMBS, read_w(to, WRAM_BOMBS) + 10);
        break;
    case TRADE_ITEM_ARROWS_5:
        write_w(from, WRAM_ARROWS, read_w(from, WRAM_ARROWS) - 5);
        write_w(to, WRAM_ARROWS, read_w(to, WRAM_ARROWS) + 5);
        break;
    case TRADE_ITEM_ARROWS_10:
        write_w(from, WRAM_ARROWS, read_w(from, WRAM_ARROWS) - 10);
        write_w(to, WRAM_ARROWS, read_w(to, WRAM_ARROWS) + 10);
        break;
    case TRADE_ITEM_POWDER_5:
        write_w(from, WRAM_POWDER, read_w(from, WRAM_POWDER) - 5);
        write_w(to, WRAM_POWDER, read_w(to, WRAM_POWDER) + 5);
        break;
    case TRADE_ITEM_HEART: {
        uint8_t hp = read_w(from, WRAM_HEALTH);
        if (hp > 8) {
            write_w(from, WRAM_HEALTH, hp - 8);
            uint8_t to_hp = read_w(to, WRAM_HEALTH);
            uint8_t to_max = read_w(to, WRAM_MAX_HEALTH);
            uint8_t new_hp = to_hp + 8;
            if (new_hp > to_max) new_hp = to_max;
            write_w(to, WRAM_HEALTH, new_hp);
        }
        break;
    }
    case TRADE_ITEM_SEASHELL:
        write_w(from, WRAM_SEASHELLS, read_w(from, WRAM_SEASHELLS) - 1);
        write_w(to, WRAM_SEASHELLS, read_w(to, WRAM_SEASHELLS) + 1);
        break;
    case TRADE_ITEM_KEY:
        write_w(from, WRAM_SMALL_KEYS, read_w(from, WRAM_SMALL_KEYS) - 1);
        write_w(to, WRAM_SMALL_KEYS, read_w(to, WRAM_SMALL_KEYS) + 1);
        break;
    default:
        break;
    }

    fprintf(stderr, "[TRADE] Executed: item %d from player to player\n", item);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void mp_trade_init(void) {
    memset(&g_trade, 0, sizeof(g_trade));
    g_trade.state = TRADE_STATE_NONE;
}

MPTradeState mp_trade_get_state(void) {
    return g_trade.state;
}

bool mp_trade_get_pending_offer(uint8_t* from_slot, uint8_t* item_type) {
    if (g_trade.state != TRADE_STATE_PENDING) return false;
    if (from_slot) *from_slot = g_trade.pending_from;
    if (item_type) *item_type = g_trade.pending_item;
    return true;
}

bool mp_trade_offer(int target_slot, MPTradeItemType item_type) {
    if (g_trade.state != TRADE_STATE_NONE) return false;
    if (target_slot < 0 || target_slot >= MP_MAX_PLAYERS) return false;

    int local = mp_session_get_local_slot();

    MPTradeOffer pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = MP_PKT_TRADE_OFFER;
    pkt.header.version = MP_PROTOCOL_VERSION;
    pkt.header.player_id = (uint8_t)local;
    pkt.from_slot = (uint8_t)local;
    pkt.to_slot = (uint8_t)target_slot;
    pkt.item_type = (uint8_t)item_type;

    /* Send via session's network context */
    /* For now, this works through the session's packet handling */
    g_trade.state = TRADE_STATE_OFFERING;
    g_trade.pending_from = (uint8_t)local;
    g_trade.pending_to = (uint8_t)target_slot;
    g_trade.pending_item = (uint8_t)item_type;

    fprintf(stderr, "[TRADE] Offering %s to player %d\n",
            mp_trade_item_names[item_type], target_slot);
    return true;
}

void mp_trade_accept(void) {
    if (g_trade.state != TRADE_STATE_PENDING) return;
    g_trade.state = TRADE_STATE_ACCEPTED;
    fprintf(stderr, "[TRADE] Accepted trade from player %d\n", g_trade.pending_from);
}

void mp_trade_decline(void) {
    if (g_trade.state != TRADE_STATE_PENDING) return;
    g_trade.state = TRADE_STATE_DECLINED;
    fprintf(stderr, "[TRADE] Declined trade from player %d\n", g_trade.pending_from);
}

void mp_trade_cancel(void) {
    if (g_trade.state == TRADE_STATE_OFFERING) {
        g_trade.state = TRADE_STATE_NONE;
        fprintf(stderr, "[TRADE] Cancelled outgoing offer\n");
    }
}

void mp_trade_handle_packet(const void* data, uint32_t size) {
    if (!data || size < sizeof(MPPacketHeader)) return;

    const MPPacketHeader* hdr = (const MPPacketHeader*)data;

    if (hdr->type == MP_PKT_TRADE_OFFER && size >= sizeof(MPTradeOffer)) {
        const MPTradeOffer* offer = (const MPTradeOffer*)data;
        int local = mp_session_get_local_slot();

        if (offer->to_slot == local) {
            g_trade.state = TRADE_STATE_PENDING;
            g_trade.pending_from = offer->from_slot;
            g_trade.pending_to = offer->to_slot;
            g_trade.pending_item = offer->item_type;

            fprintf(stderr, "[TRADE] Received offer: %s from player %d\n",
                    (offer->item_type < TRADE_ITEM_COUNT)
                        ? mp_trade_item_names[offer->item_type] : "???",
                    offer->from_slot);
        }

    } else if (hdr->type == MP_PKT_TRADE_RESPONSE && size >= sizeof(MPTradeResponse)) {
        const MPTradeResponse* resp = (const MPTradeResponse*)data;

        if (resp->accepted) {
            g_trade.state = TRADE_STATE_ACCEPTED;
            fprintf(stderr, "[TRADE] Trade accepted by player %d\n", resp->to_slot);
        } else {
            g_trade.state = TRADE_STATE_DECLINED;
            fprintf(stderr, "[TRADE] Trade declined by player %d\n", resp->to_slot);
        }
    }
}

void mp_trade_reset(void) {
    g_trade.state = TRADE_STATE_NONE;
}
