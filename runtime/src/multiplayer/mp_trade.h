/**
 * @file mp_trade.h
 * @brief Item trading between players
 *
 * Players can trade inventory items with each other. One player initiates
 * a trade offer (specifying item and target player), the other accepts
 * or declines. On accept, items are swapped in both players' WRAM.
 *
 * Trade flow:
 * 1. Player A opens trade menu, selects item and target player
 * 2. Trade offer sent to host (or directly if host)
 * 3. Target player sees popup with offer
 * 4. Accept: items transferred in both contexts
 * 5. Decline: both players notified
 */

#ifndef MP_TRADE_H
#define MP_TRADE_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Tradeable Item Types
 * ========================================================================== */

typedef enum {
    TRADE_ITEM_RUPEES_50   = 0,   /* 50 rupees */
    TRADE_ITEM_RUPEES_100  = 1,   /* 100 rupees */
    TRADE_ITEM_RUPEES_200  = 2,   /* 200 rupees */
    TRADE_ITEM_BOMBS_5     = 3,   /* 5 bombs */
    TRADE_ITEM_BOMBS_10    = 4,   /* 10 bombs */
    TRADE_ITEM_ARROWS_5    = 5,   /* 5 arrows */
    TRADE_ITEM_ARROWS_10   = 6,   /* 10 arrows */
    TRADE_ITEM_POWDER_5    = 7,   /* 5 magic powder */
    TRADE_ITEM_HEART       = 8,   /* 1 heart (8 HP) */
    TRADE_ITEM_SEASHELL    = 9,   /* 1 secret seashell */
    TRADE_ITEM_KEY         = 10,  /* 1 small key (current dungeon) */
    TRADE_ITEM_COUNT
} MPTradeItemType;

static const char* mp_trade_item_names[] = {
    "50 Rupees", "100 Rupees", "200 Rupees",
    "5 Bombs", "10 Bombs",
    "5 Arrows", "10 Arrows",
    "5 Magic Powder",
    "1 Heart",
    "1 Secret Seashell",
    "1 Small Key"
};

/* ============================================================================
 * Trade State
 * ========================================================================== */

typedef enum {
    TRADE_STATE_NONE,        /* No active trade */
    TRADE_STATE_OFFERING,    /* We sent an offer, waiting for response */
    TRADE_STATE_PENDING,     /* We received an offer, awaiting our decision */
    TRADE_STATE_ACCEPTED,    /* Trade completed successfully */
    TRADE_STATE_DECLINED,    /* Trade was declined */
} MPTradeState;

/* ============================================================================
 * Trade Packets
 * ========================================================================== */

#pragma pack(push, 1)

typedef struct {
    MPPacketHeader header;
    uint8_t  from_slot;       /* Offering player slot */
    uint8_t  to_slot;         /* Target player slot */
    uint8_t  item_type;       /* MPTradeItemType */
    uint8_t  _pad;
} MPTradeOffer;

typedef struct {
    MPPacketHeader header;
    uint8_t  from_slot;       /* Original offerer */
    uint8_t  to_slot;         /* Responder */
    uint8_t  accepted;        /* 1 = accepted, 0 = declined */
    uint8_t  item_type;       /* Item being traded */
} MPTradeResponse;

#pragma pack(pop)

/* Packet type IDs (extend MPPacketType) */
#define MP_PKT_TRADE_OFFER    0x60
#define MP_PKT_TRADE_RESPONSE 0x61

/* ============================================================================
 * Trade API
 * ========================================================================== */

/** Initialize trade system */
void mp_trade_init(void);

/** Get current trade state */
MPTradeState mp_trade_get_state(void);

/** Get pending trade offer (when state == TRADE_STATE_PENDING) */
bool mp_trade_get_pending_offer(uint8_t* from_slot, uint8_t* item_type);

/**
 * Send a trade offer to another player.
 * @param target_slot  Player to trade with (1-3)
 * @param item_type    What to offer
 * @return true if offer was sent
 */
bool mp_trade_offer(int target_slot, MPTradeItemType item_type);

/** Accept the pending trade offer */
void mp_trade_accept(void);

/** Decline the pending trade offer */
void mp_trade_decline(void);

/** Cancel our outgoing offer */
void mp_trade_cancel(void);

/**
 * Process a received trade packet.
 * Called by mp_session when a trade packet arrives.
 */
void mp_trade_handle_packet(const void* data, uint32_t size);

/**
 * Execute the actual item transfer between two contexts.
 * Called on the host when a trade is accepted.
 */
void mp_trade_execute(void* ctx_from, void* ctx_to, MPTradeItemType item_type);

/** Reset trade state (clear any pending offers) */
void mp_trade_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_TRADE_H */
