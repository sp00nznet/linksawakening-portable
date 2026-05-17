/**
 * @file mp_session.cpp
 * @brief Multiplayer session management implementation
 */

#include "mp_session.h"
#include "mp_net.h"
#include "mp_protocol.h"
#include "mp_worldsync.h"
#include "mp_color.h"
#include "mp_sprites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "gbrt.h"
#include "ppu.h"
}

/* ============================================================================
 * Session State
 * ========================================================================== */

static struct {
    MPSessionState state;
    MPNetContext*   net;
    bool           is_host;

    /* Player data */
    MPPlayer       players[MP_MAX_PLAYERS];
    int            local_slot;       /* Our player slot */
    int            player_count;

    /* Settings */
    char           local_name[MP_MAX_NAME_LEN];
    float          local_color_h;
    float          local_color_s;
    float          local_color_v;
    bool           debug_overlay;
    int            min_hearts;   /* Minimum hearts for joining players (0=use save) */

    /* Starting equipment for joining players */
    int            start_sword;    /* 0=none, 1=L1, 2=L2 */
    int            start_shield;   /* 0=none, 1=L1, 2=mirror */
    int            start_bracelet; /* 0=none, 1=L1, 2=L2 */
    bool           start_boots;
    bool           start_flippers;
    bool           start_feather;
    bool           start_bow;
    bool           start_hookshot;
    bool           start_rod;
    bool           start_ocarina;
    bool           start_shovel;
    int            start_rupees;
    int            start_bombs;
    int            start_arrows;
    int            start_powder;

    /* Client: received framebuffer */
    uint32_t       client_framebuffer[MP_FRAME_PIXELS];
    bool           client_has_frame;

    /* Frame counter */
    uint32_t       frame_num;
    uint32_t       packet_seq;

    /* Compression buffer for frame sending */
    uint8_t        compress_buf[MP_FRAME_BYTES * 2]; /* worst case RLE */

} g_session = {
    .state = MP_STATE_IDLE,
    .local_name = "Link",
    .local_color_h = 120.0f,  /* Green (default Link) */
    .local_color_s = 0.8f,
    .local_color_v = 0.8f,
};

/* ============================================================================
 * WRAM addresses to sync between players (shared world state)
 * ========================================================================== */

/* These are key LADX WRAM addresses that represent world state.
 * When any player changes one of these, it gets synced to all others. */
static const uint16_t g_sync_addresses[] = {
    /* Dungeon item flags, keys, etc. would go here */
    /* For now, we sync a minimal set - this will grow */
    0xDB86,  /* Keys (current dungeon) */
    0xDB87,  /* Nightmare key */
    0xDB88,  /* Dungeon map */
    0xDB89,  /* Compass */
    0xDB8A,  /* Stone beak */
};

static const int g_sync_address_count = sizeof(g_sync_addresses) / sizeof(g_sync_addresses[0]);

/* ============================================================================
 * Network Callbacks
 * ========================================================================== */

/* LADX WRAM addresses for Link's position */
#define LADX_LINK_X       0xD100
#define LADX_LINK_Y       0xD110
#define LADX_MAP_ROOM     0xD401
#define LADX_IS_INDOOR    0xD402
#define LADX_DUNGEON_IDX  0xDB83
#define LADX_LINK_HEALTH  0xDB5A
#define LADX_LINK_MAX_HP  0xDB5B

/* Forward declarations */
static uint8_t read_ctx_wram(GBContext* ctx, uint16_t addr);
static void write_ctx_wram(GBContext* ctx, uint16_t addr, uint8_t value);

/* Memory sizes (must match gbrt.c allocations) */
#define SS_WRAM_SIZE   (0x1000 * 8)
#define SS_VRAM_SIZE   (0x2000 * 2)
#define SS_OAM_SIZE    0xA0
#define SS_HRAM_SIZE   0x7F
#define SS_IO_SIZE     0x81

/**
 * Clone a running GBContext into a freshly-created one.
 * Copies all CPU state, memory banks, and PPU state so the clone
 * is an exact snapshot of the source — ready to run independently.
 */
static void clone_context(GBContext* dst, const GBContext* src) {
    if (!dst || !src) return;

    /* CPU registers and state */
    dst->af = src->af;
    dst->bc = src->bc;
    dst->de = src->de;
    dst->hl = src->hl;
    dst->sp = src->sp;
    dst->pc = src->pc;
    dst->f_z = src->f_z;
    dst->f_n = src->f_n;
    dst->f_h = src->f_h;
    dst->f_c = src->f_c;
    dst->ime = src->ime;
    dst->ime_pending = src->ime_pending;
    dst->halted = src->halted;
    dst->stopped = src->stopped;
    dst->halt_bug = src->halt_bug;
    dst->speed_switch_halt = src->speed_switch_halt;
    dst->dma = src->dma;

    /* Bank state */
    dst->rom_bank = src->rom_bank;
    dst->ram_bank = src->ram_bank;
    dst->wram_bank = src->wram_bank;
    dst->vram_bank = src->vram_bank;

    /* MBC state */
    dst->mbc_type = src->mbc_type;
    dst->ram_enabled = src->ram_enabled;
    dst->mbc_mode = src->mbc_mode;
    dst->rom_bank_upper = src->rom_bank_upper;
    dst->rtc_mode = src->rtc_mode;
    dst->rtc_reg = src->rtc_reg;

    /* Timing */
    dst->cycles = src->cycles;
    dst->frame_cycles = src->frame_cycles;
    dst->last_sync_cycles = src->last_sync_cycles;
    dst->frame_done = src->frame_done;
    dst->div_counter = src->div_counter;
    dst->last_joypad = src->last_joypad;

    /* Memory regions */
    if (dst->wram && src->wram)
        memcpy(dst->wram, src->wram, SS_WRAM_SIZE);
    if (dst->vram && src->vram)
        memcpy(dst->vram, src->vram, SS_VRAM_SIZE);
    if (dst->oam && src->oam)
        memcpy(dst->oam, src->oam, SS_OAM_SIZE);
    if (dst->hram && src->hram)
        memcpy(dst->hram, src->hram, SS_HRAM_SIZE);
    if (dst->io && src->io)
        memcpy(dst->io, src->io, SS_IO_SIZE);

    /* External RAM */
    if (dst->eram && src->eram && src->eram_size > 0 &&
        dst->eram_size >= src->eram_size) {
        memcpy(dst->eram, src->eram, src->eram_size);
    }

    /* PPU state */
    if (dst->ppu && src->ppu) {
        /* Copy PPU struct (includes framebuffer, scanline state, palettes) */
        memcpy(dst->ppu, src->ppu, sizeof(GBPPU));
    }

    fprintf(stderr, "[MP_SESSION] Context cloned (PC=%04X bank=%d room=%02X)\n",
            dst->pc, dst->rom_bank,
            read_ctx_wram(dst, LADX_MAP_ROOM));
}

/* ============================================================================
 * LADX WRAM addresses for player-specific state
 * ========================================================================== */

/* ============================================================================
 * LADX WRAM: Inventory System
 * ========================================================================== */

/* Inventory grid: B button, A button, and 10 subscreen slots */
#define LADX_INV_B            0xDB00  /* Item ID equipped to B button */
#define LADX_INV_A            0xDB01  /* Item ID equipped to A button */
#define LADX_INV_GRID         0xDB02  /* 10 inventory slots (DB02-DB0B) */
#define LADX_INV_COUNT        0xDB0C  /* Number of items in inventory */

/* Item IDs used in inventory grid slots */
#define ITEM_SWORD            0x01
#define ITEM_BOMBS            0x02
#define ITEM_BRACELET         0x03
#define ITEM_SHIELD           0x04
#define ITEM_BOW              0x05
#define ITEM_HOOKSHOT         0x06
#define ITEM_ROD              0x07
#define ITEM_BOOTS            0x08
#define ITEM_OCARINA          0x09
#define ITEM_FEATHER          0x0A
#define ITEM_SHOVEL           0x0B
#define ITEM_POWDER           0x0C

/* Item data addresses (levels, counts, possession flags) */
#define LADX_SWORD_LEVEL      0xDB4E  /* 0=none, 1=L1, 2=L2 */
#define LADX_SHIELD_LEVEL     0xDB44  /* 0=none, 1=L1, 2=mirror */
#define LADX_BRACELET_LEVEL   0xDB43  /* 0=none, 1=L1, 2=L2 */
#define LADX_HAS_FLIPPERS     0xDB3E  /* 0/1 (passive, not in grid) */
#define LADX_HAS_BOOTS        0xDB4A  /* 0/1 */
#define LADX_HAS_FEATHER      0xDB4F  /* 0/1 */
#define LADX_HAS_OCARINA      0xDB49  /* 0/1 */
#define LADX_HAS_ROD          0xDB48  /* 0/1 */
#define LADX_HAS_HOOKSHOT     0xDB4B  /* 0/1 */
#define LADX_RUPEES_HIGH      0xDB5D
#define LADX_RUPEES_LOW       0xDB5E
#define LADX_BOMBS            0xDB4D
#define LADX_ARROWS           0xDB45
#define LADX_POWDER           0xDB4C

/* Link's position in the entity table (X table at D100, Y table at D110) */
#define LADX_ENTITY_X         0xD100
#define LADX_ENTITY_Y         0xD110

/* Gameplay type */
#define LADX_GAMEPLAY_TYPE    0xDB95
#define LADX_GAMEPLAY_SUBTYPE 0xDB96

/**
 * Reset player-specific state after cloning from host.
 * Rebuilds the inventory grid to match the host's configured starting
 * equipment, so the subscreen stays consistent with item flags.
 */
static void setup_new_player(GBContext* ctx, int slot) {
    if (!ctx) return;

    /* ---- Health ---- */
    int hearts = g_session.min_hearts > 0 ? g_session.min_hearts : 3;
    uint8_t hp = (uint8_t)(hearts * 8);
    write_ctx_wram(ctx, LADX_LINK_HEALTH, hp);
    write_ctx_wram(ctx, LADX_LINK_MAX_HP, hp);

    /* ---- Build inventory grid from configured items ---- */
    uint8_t grid[10];
    int grid_count = 0;
    uint8_t a_item = 0;
    uint8_t b_item = 0;

    /* Sword (goes in A button) */
    write_ctx_wram(ctx, LADX_SWORD_LEVEL, (uint8_t)g_session.start_sword);
    if (g_session.start_sword > 0) {
        a_item = ITEM_SWORD;
        grid[grid_count++] = ITEM_SWORD;
    }

    /* Shield */
    write_ctx_wram(ctx, LADX_SHIELD_LEVEL, (uint8_t)g_session.start_shield);
    if (g_session.start_shield > 0 && grid_count < 10)
        grid[grid_count++] = ITEM_SHIELD;

    /* Power Bracelet */
    write_ctx_wram(ctx, LADX_BRACELET_LEVEL, (uint8_t)g_session.start_bracelet);
    if (g_session.start_bracelet > 0 && grid_count < 10)
        grid[grid_count++] = ITEM_BRACELET;

    /* Roc's Feather */
    write_ctx_wram(ctx, LADX_HAS_FEATHER, g_session.start_feather ? 1 : 0);
    if (g_session.start_feather && grid_count < 10)
        grid[grid_count++] = ITEM_FEATHER;

    /* Pegasus Boots */
    write_ctx_wram(ctx, LADX_HAS_BOOTS, g_session.start_boots ? 1 : 0);
    if (g_session.start_boots && grid_count < 10)
        grid[grid_count++] = ITEM_BOOTS;

    /* Bow (needs arrows to be useful) */
    if (g_session.start_bow && grid_count < 10)
        grid[grid_count++] = ITEM_BOW;

    /* Hookshot */
    write_ctx_wram(ctx, LADX_HAS_HOOKSHOT, g_session.start_hookshot ? 1 : 0);
    if (g_session.start_hookshot && grid_count < 10)
        grid[grid_count++] = ITEM_HOOKSHOT;

    /* Magic Rod */
    write_ctx_wram(ctx, LADX_HAS_ROD, g_session.start_rod ? 1 : 0);
    if (g_session.start_rod && grid_count < 10)
        grid[grid_count++] = ITEM_ROD;

    /* Ocarina */
    write_ctx_wram(ctx, LADX_HAS_OCARINA, g_session.start_ocarina ? 1 : 0);
    if (g_session.start_ocarina && grid_count < 10)
        grid[grid_count++] = ITEM_OCARINA;

    /* Shovel */
    if (g_session.start_shovel && grid_count < 10)
        grid[grid_count++] = ITEM_SHOVEL;

    /* Bombs (add to grid if count > 0) */
    int bombs = g_session.start_bombs;
    write_ctx_wram(ctx, LADX_BOMBS, (uint8_t)bombs);
    if (bombs > 0 && grid_count < 10)
        grid[grid_count++] = ITEM_BOMBS;

    /* Magic Powder (add to grid if count > 0) */
    int powder = g_session.start_powder;
    write_ctx_wram(ctx, LADX_POWDER, (uint8_t)powder);
    if (powder > 0 && grid_count < 10)
        grid[grid_count++] = ITEM_POWDER;

    /* Arrows (count only, bow is the grid item) */
    write_ctx_wram(ctx, LADX_ARROWS, (uint8_t)g_session.start_arrows);

    /* Flippers (passive item, no grid slot) */
    write_ctx_wram(ctx, LADX_HAS_FLIPPERS, g_session.start_flippers ? 1 : 0);

    /* Rupees */
    int rupees = g_session.start_rupees;
    write_ctx_wram(ctx, LADX_RUPEES_HIGH, (uint8_t)((rupees >> 8) & 0xFF));
    write_ctx_wram(ctx, LADX_RUPEES_LOW,  (uint8_t)(rupees & 0xFF));

    /* ---- Write inventory grid ---- */
    for (int i = 0; i < 10; i++)
        write_ctx_wram(ctx, LADX_INV_GRID + i, i < grid_count ? grid[i] : 0x00);

    /* A button = sword (or first item) */
    if (a_item == 0 && grid_count > 0)
        a_item = grid[0];
    write_ctx_wram(ctx, LADX_INV_A, a_item);

    /* B button = first non-sword item */
    for (int i = 0; i < grid_count; i++) {
        if (grid[i] != ITEM_SWORD) { b_item = grid[i]; break; }
    }
    if (b_item == 0 && grid_count > 0) b_item = grid[0];
    write_ctx_wram(ctx, LADX_INV_B, b_item);

    /* Inventory count */
    write_ctx_wram(ctx, LADX_INV_COUNT, (uint8_t)grid_count);

    fprintf(stderr, "[MP_SESSION] Player %d setup: %d hearts, %d items in grid, "
            "sword=%d, shield=%d, bracelet=%d\n",
            slot, hearts, grid_count,
            g_session.start_sword, g_session.start_shield, g_session.start_bracelet);
}

static uint8_t read_ctx_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000)
        return ctx->wram[addr - 0xC000];
    return 0;
}

static void write_ctx_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (!ctx || !ctx->wram) return;
    if (addr >= 0xC000 && addr < 0xE000)
        ctx->wram[addr - 0xC000] = value;
}

static void update_player_from_ctx(MPPlayer* player, GBContext* ctx) {
    if (!player || !ctx) return;
    player->map_room    = read_ctx_wram(ctx, LADX_MAP_ROOM);
    player->is_indoor   = read_ctx_wram(ctx, LADX_IS_INDOOR);
    player->dungeon_idx = read_ctx_wram(ctx, LADX_DUNGEON_IDX);
    player->health      = read_ctx_wram(ctx, LADX_LINK_HEALTH);
    player->max_health  = read_ctx_wram(ctx, LADX_LINK_MAX_HP);
    player->link_x      = read_ctx_wram(ctx, LADX_LINK_X);
    player->link_y      = read_ctx_wram(ctx, LADX_LINK_Y);
}

static MPPacketHeader make_header(uint8_t type) {
    MPPacketHeader hdr;
    hdr.type = type;
    hdr.version = MP_PROTOCOL_VERSION;
    hdr.player_id = (uint8_t)g_session.local_slot;
    hdr._pad = 0;
    hdr.sequence = g_session.packet_seq++;
    return hdr;
}

/* ---- Host callbacks ---- */

static void host_on_connect(int client_slot) {
    fprintf(stderr, "[MP_SESSION] Client connected to slot %d\n", client_slot);
    /* Actual player setup happens when we receive JOIN_REQ */
}

static void host_on_disconnect(int client_slot) {
    /* Client slots are 0-based for network (slots 0-2),
     * but map to player slots 1-3 (host is player 0) */
    int player_slot = client_slot + 1;
    fprintf(stderr, "[MP_SESSION] Player %d disconnected\n", player_slot);

    MPPlayer* p = &g_session.players[player_slot];
    if (p->active) {
        p->active = false;
        if (p->ctx) {
            gb_context_destroy(p->ctx);
            p->ctx = NULL;
        }
        g_session.player_count--;
    }
}

static void host_on_receive(int client_slot, const void* data, uint32_t size) {
    if (size < sizeof(MPPacketHeader)) return;

    const MPPacketHeader* hdr = (const MPPacketHeader*)data;
    int player_slot = client_slot + 1;

    switch (hdr->type) {
    case MP_PKT_JOIN_REQ: {
        if (size < sizeof(MPJoinReq)) return;
        const MPJoinReq* req = (const MPJoinReq*)data;

        MPPlayer* p = &g_session.players[player_slot];
        p->active = true;
        strncpy(p->name, req->name, MP_MAX_NAME_LEN - 1);
        p->name[MP_MAX_NAME_LEN - 1] = '\0';
        p->color_h = req->color_h;
        p->color_s = req->color_s;
        p->color_v = req->color_v;
        p->input_dpad = 0xFF;
        p->input_buttons = 0xFF;

        /* Create a new GBContext by cloning the host's running state.
         * This gives the new player an exact copy of the game in its
         * current state — same room, same world, ready to run. */
        if (!p->ctx) {
            GBConfig config = {};
            config.model = GB_MODEL_CGB;
            config.enable_bootrom = false;
            config.enable_audio = false; /* Don't play audio for remote players */
            config.enable_serial = false;
            config.speed_percent = 100;
            p->ctx = gb_context_create(&config);
            if (p->ctx && g_session.players[0].ctx) {
                GBContext* host_ctx = g_session.players[0].ctx;

                /* Load ROM first (allocates ERAM based on cart header) */
                gb_context_load_rom(p->ctx, host_ctx->rom, host_ctx->rom_size);

                /* Clone entire running state from host */
                clone_context(p->ctx, host_ctx);

                /* Set up player-specific inventory from host config.
                 * Rebuilds inventory grid to match configured items. */
                setup_new_player(p->ctx, player_slot);

                fprintf(stderr, "[MP_SESSION] Player '%s' context ready (cloned + custom gear)\n",
                        p->name);
            }
        }

        g_session.player_count++;

        /* Send acknowledgment */
        MPJoinAck ack;
        memset(&ack, 0, sizeof(ack));
        ack.header = make_header(MP_PKT_JOIN_ACK);
        ack.assigned_slot = (uint8_t)player_slot;
        ack.total_players = (uint8_t)g_session.player_count;
        mp_net_host_send(g_session.net, client_slot, &ack, sizeof(ack),
                         MP_CHANNEL_RELIABLE);

        fprintf(stderr, "[MP_SESSION] Player '%s' joined as slot %d (color H=%.0f)\n",
                p->name, player_slot, p->color_h);
        break;
    }

    case MP_PKT_INPUT: {
        if (size < sizeof(MPInputPacket)) return;
        const MPInputPacket* input = (const MPInputPacket*)data;

        MPPlayer* p = &g_session.players[player_slot];
        if (p->active) {
            p->input_dpad = input->joypad_dpad;
            p->input_buttons = input->joypad_buttons;
        }
        break;
    }

    case MP_PKT_DISCONNECT: {
        host_on_disconnect(client_slot);
        break;
    }

    default:
        fprintf(stderr, "[MP_SESSION] Unknown packet type 0x%02X from slot %d\n",
                hdr->type, player_slot);
        break;
    }
}

/* ---- Client callbacks ---- */

static void client_on_connect(int slot) {
    (void)slot;
    fprintf(stderr, "[MP_SESSION] Connected to host, sending join request...\n");
    g_session.state = MP_STATE_CONNECTED;

    /* Send join request */
    MPJoinReq req;
    memset(&req, 0, sizeof(req));
    req.header = make_header(MP_PKT_JOIN_REQ);
    strncpy(req.name, g_session.local_name, MP_MAX_NAME_LEN - 1);
    req.color_h = g_session.local_color_h;
    req.color_s = g_session.local_color_s;
    req.color_v = g_session.local_color_v;

    mp_net_client_send(g_session.net, &req, sizeof(req), MP_CHANNEL_RELIABLE);
}

static void client_on_disconnect(int slot) {
    (void)slot;
    fprintf(stderr, "[MP_SESSION] Disconnected from host\n");
    g_session.state = MP_STATE_DISCONNECTED;
}

static void client_on_receive(int slot, const void* data, uint32_t size) {
    (void)slot;
    if (size < sizeof(MPPacketHeader)) return;

    const MPPacketHeader* hdr = (const MPPacketHeader*)data;

    switch (hdr->type) {
    case MP_PKT_JOIN_ACK: {
        if (size < sizeof(MPJoinAck)) return;
        const MPJoinAck* ack = (const MPJoinAck*)data;
        g_session.local_slot = ack->assigned_slot;
        g_session.player_count = ack->total_players;
        fprintf(stderr, "[MP_SESSION] Joined as player %d (%d players total)\n",
                g_session.local_slot, g_session.player_count);
        break;
    }

    case MP_PKT_JOIN_DENY: {
        if (size < sizeof(MPJoinDeny)) return;
        const MPJoinDeny* deny = (const MPJoinDeny*)data;
        fprintf(stderr, "[MP_SESSION] Join denied: %s\n", deny->message);
        g_session.state = MP_STATE_DISCONNECTED;
        break;
    }

    case MP_PKT_FRAME: {
        if (size < sizeof(MPFramePacket)) return;
        const MPFramePacket* frame = (const MPFramePacket*)data;
        const uint8_t* compressed = (const uint8_t*)data + sizeof(MPFramePacket);
        uint32_t comp_size = frame->compressed_size;

        if (sizeof(MPFramePacket) + comp_size > size) return; /* truncated */

        int pixels = mp_rle_decompress(compressed, comp_size,
                                        g_session.client_framebuffer,
                                        MP_FRAME_PIXELS);
        if (pixels == MP_FRAME_PIXELS)
            g_session.client_has_frame = true;
        break;
    }

    case MP_PKT_PLAYER_INFO: {
        if (size < sizeof(MPPlayerInfo)) return;
        const MPPlayerInfo* info = (const MPPlayerInfo*)data;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            MPPlayer* p = &g_session.players[i];
            const MPPlayerState* ps = &info->players[i];
            p->active = ps->connected;
            memcpy(p->name, ps->name, MP_MAX_NAME_LEN);
            p->color_h = ps->color_h;
            p->color_s = ps->color_s;
            p->color_v = ps->color_v;
            p->map_room = ps->map_room;
            p->is_indoor = ps->is_indoor;
            p->dungeon_idx = ps->dungeon_idx;
            p->health = ps->health;
            p->max_health = ps->max_health;
            p->link_x = ps->link_x;
            p->link_y = ps->link_y;
            p->ping_ms = ps->ping_ms;
        }
        break;
    }

    case MP_PKT_WORLD_SYNC: {
        /* TODO: Apply world state changes to local view */
        break;
    }

    default:
        break;
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void mp_session_init(void) {
    memset(&g_session, 0, sizeof(g_session));
    g_session.state = MP_STATE_IDLE;
    strncpy(g_session.local_name, "Link", MP_MAX_NAME_LEN);
    g_session.local_color_h = 120.0f;
    g_session.local_color_s = 0.8f;
    g_session.local_color_v = 0.8f;
    g_session.debug_overlay = false;

    /* Default starting equipment */
    g_session.min_hearts = 3;
    g_session.start_sword = 1;
    g_session.start_shield = 1;
    g_session.start_bracelet = 0;
    g_session.start_feather = true;
    g_session.start_boots = false;
    g_session.start_flippers = false;
    g_session.start_bow = false;
    g_session.start_hookshot = false;
    g_session.start_rod = false;
    g_session.start_ocarina = false;
    g_session.start_shovel = false;
    g_session.start_rupees = 100;
    g_session.start_bombs = 10;
    g_session.start_arrows = 10;
    g_session.start_powder = 10;

    mp_net_init();
    mp_worldsync_init();
    mp_color_set_reference_palette();
}

void mp_session_shutdown(void) {
    mp_session_leave();
    mp_net_shutdown();
}

MPSessionState mp_session_get_state(void) {
    return g_session.state;
}

bool mp_session_host(GBContext* host_ctx, uint16_t port) {
    if (g_session.state != MP_STATE_IDLE) {
        fprintf(stderr, "[MP_SESSION] Cannot host: already in a session\n");
        return false;
    }

    if (port == 0) port = MP_DEFAULT_PORT;

    g_session.net = mp_net_host_create(port);
    if (!g_session.net) return false;

    mp_net_set_callbacks(g_session.net,
                         host_on_connect, host_on_disconnect, host_on_receive);

    /* Player 0 = host */
    g_session.is_host = true;
    g_session.local_slot = 0;
    g_session.player_count = 1;

    MPPlayer* p0 = &g_session.players[0];
    p0->active = true;
    p0->ctx = host_ctx;
    strncpy(p0->name, g_session.local_name, MP_MAX_NAME_LEN);
    p0->color_h = g_session.local_color_h;
    p0->color_s = g_session.local_color_s;
    p0->color_v = g_session.local_color_v;

    g_session.state = MP_STATE_HOSTING;
    fprintf(stderr, "[MP_SESSION] Hosting on port %u as '%s'\n",
            port, g_session.local_name);
    return true;
}

bool mp_session_join(const char* address, uint16_t port,
                     const char* name, float h, float s, float v)
{
    if (g_session.state != MP_STATE_IDLE) return false;
    if (port == 0) port = MP_DEFAULT_PORT;

    strncpy(g_session.local_name, name, MP_MAX_NAME_LEN - 1);
    g_session.local_color_h = h;
    g_session.local_color_s = s;
    g_session.local_color_v = v;

    g_session.net = mp_net_client_create(address, port);
    if (!g_session.net) return false;

    mp_net_set_callbacks(g_session.net,
                         client_on_connect, client_on_disconnect, client_on_receive);

    g_session.is_host = false;
    g_session.local_slot = -1; /* Assigned by host */
    g_session.state = MP_STATE_CONNECTING;

    fprintf(stderr, "[MP_SESSION] Connecting to %s:%u as '%s'...\n",
            address, port, name);
    return true;
}

void mp_session_leave(void) {
    if (g_session.state == MP_STATE_IDLE) return;

    if (g_session.is_host) {
        /* Destroy all non-host contexts */
        for (int i = 1; i < MP_MAX_PLAYERS; i++) {
            if (g_session.players[i].ctx) {
                gb_context_destroy(g_session.players[i].ctx);
                g_session.players[i].ctx = NULL;
            }
            g_session.players[i].active = false;
        }
        /* Don't destroy host context - it belongs to the caller */
        g_session.players[0].ctx = NULL;
    }

    if (g_session.net) {
        mp_net_destroy(g_session.net);
        g_session.net = NULL;
    }

    memset(g_session.players, 0, sizeof(g_session.players));
    g_session.state = MP_STATE_IDLE;
    g_session.player_count = 0;

    fprintf(stderr, "[MP_SESSION] Left multiplayer session\n");
}

void mp_session_update(void) {
    if (g_session.state == MP_STATE_IDLE) return;

    /* Service the network (0ms timeout = non-blocking) */
    if (g_session.is_host) {
        mp_net_host_service(g_session.net, 0);
    } else {
        mp_net_client_service(g_session.net, 0);
    }

    g_session.frame_num++;

    if (g_session.is_host) {
        /* ---- HOST: Run frames for all connected player contexts ---- */

        /* Update host player state from host context */
        if (g_session.players[0].ctx) {
            update_player_from_ctx(&g_session.players[0], g_session.players[0].ctx);
        }

        /* Run frames for connected clients */
        for (int i = 1; i < MP_MAX_PLAYERS; i++) {
            MPPlayer* p = &g_session.players[i];
            if (!p->active || !p->ctx) continue;

            /* Apply client input to their joypad */
            extern uint8_t g_joypad_buttons;
            extern uint8_t g_joypad_dpad;

            /* Temporarily swap joypad state to run this player's frame */
            uint8_t saved_dpad = g_joypad_dpad;
            uint8_t saved_buttons = g_joypad_buttons;

            g_joypad_dpad = p->input_dpad;
            g_joypad_buttons = p->input_buttons;

            /* Run one frame */
            gb_run_frame(p->ctx);

            /* Restore host joypad */
            g_joypad_dpad = saved_dpad;
            g_joypad_buttons = saved_buttons;

            /* Update player state */
            update_player_from_ctx(p, p->ctx);
            p->ping_ms = mp_net_host_get_rtt(g_session.net, i - 1);

            /* Send framebuffer to client (with color recoloring + remote sprites) */
            const uint32_t* fb = gb_get_framebuffer(p->ctx);
            if (fb) {
                /* Apply player's chosen color to their Link */
                static uint32_t fb_colored[MP_FRAME_PIXELS];
                memcpy(fb_colored, fb, MP_FRAME_BYTES);
                mp_color_recolor_link(fb_colored, p->color_h, p->color_s, p->color_v);

                /* Composite other players' Link sprites onto this framebuffer */
                for (int j = 0; j < MP_MAX_PLAYERS; j++) {
                    if (j == i) continue;
                    MPPlayer* other = &g_session.players[j];
                    if (!other->active || !other->ctx) continue;
                    mp_sprites_composite_player(fb_colored, p->ctx, other->ctx,
                                                other->color_h, other->color_s,
                                                other->color_v);
                }

                uint32_t comp_size = mp_rle_compress(
                    fb_colored, MP_FRAME_PIXELS,
                    g_session.compress_buf + sizeof(MPFramePacket),
                    sizeof(g_session.compress_buf) - sizeof(MPFramePacket));

                if (comp_size > 0) {
                    MPFramePacket* pkt = (MPFramePacket*)g_session.compress_buf;
                    pkt->header = make_header(MP_PKT_FRAME);
                    pkt->frame_num = g_session.frame_num;
                    pkt->compressed_size = comp_size;

                    mp_net_host_send(g_session.net, i - 1,
                                     g_session.compress_buf,
                                     sizeof(MPFramePacket) + comp_size,
                                     MP_CHANNEL_UNRELIABLE);
                }

                gb_reset_frame(p->ctx);
            }
        }

        /* ---- World sync: deferred OR-merge after ALL frames complete ---- */
        if (g_session.player_count > 1) {
            GBContext* sync_contexts[MP_MAX_PLAYERS];
            int sync_count = 0;
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                if (g_session.players[i].active && g_session.players[i].ctx)
                    sync_contexts[sync_count++] = g_session.players[i].ctx;
            }
            if (sync_count > 1)
                mp_worldsync_merge_all(sync_contexts, sync_count);
        }

        /* Broadcast player info every 10 frames */
        if (g_session.frame_num % 10 == 0 && g_session.player_count > 1) {
            MPPlayerInfo info;
            memset(&info, 0, sizeof(info));
            info.header = make_header(MP_PKT_PLAYER_INFO);
            info.frame_num = g_session.frame_num;

            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                MPPlayer* p = &g_session.players[i];
                MPPlayerState* ps = &info.players[i];
                ps->slot = (uint8_t)i;
                ps->connected = p->active ? 1 : 0;
                memcpy(ps->name, p->name, MP_MAX_NAME_LEN);
                ps->color_h = p->color_h;
                ps->color_s = p->color_s;
                ps->color_v = p->color_v;
                ps->map_room = p->map_room;
                ps->is_indoor = p->is_indoor;
                ps->dungeon_idx = p->dungeon_idx;
                ps->health = p->health;
                ps->max_health = p->max_health;
                ps->link_x = p->link_x;
                ps->link_y = p->link_y;
                ps->ping_ms = p->ping_ms;
            }

            mp_net_host_broadcast(g_session.net, &info, sizeof(info),
                                  MP_CHANNEL_UNRELIABLE);
        }

    } else {
        /* ---- CLIENT: Send input to host ---- */
        if (g_session.state == MP_STATE_CONNECTED) {
            extern uint8_t g_joypad_buttons;
            extern uint8_t g_joypad_dpad;

            MPInputPacket input;
            memset(&input, 0, sizeof(input));
            input.header = make_header(MP_PKT_INPUT);
            input.joypad_dpad = g_joypad_dpad;
            input.joypad_buttons = g_joypad_buttons;
            input.frame_num = g_session.frame_num;

            mp_net_client_send(g_session.net, &input, sizeof(input),
                               MP_CHANNEL_UNRELIABLE);
        }
    }
}

const MPPlayer* mp_session_get_player(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return NULL;
    if (!g_session.players[slot].active) return NULL;
    return &g_session.players[slot];
}

int mp_session_get_local_slot(void) {
    return g_session.local_slot;
}

int mp_session_get_player_count(void) {
    return g_session.player_count;
}

const uint32_t* mp_session_get_framebuffer(void) {
    if (g_session.is_host) {
        if (!g_session.players[0].ctx) return NULL;

        const uint32_t* raw_fb = gb_get_framebuffer(g_session.players[0].ctx);
        if (!raw_fb) return NULL;

        /* If we have other players, composite their sprites onto the host view */
        if (g_session.player_count > 1) {
            static uint32_t host_fb[MP_FRAME_PIXELS];
            memcpy(host_fb, raw_fb, MP_FRAME_BYTES);

            for (int j = 1; j < MP_MAX_PLAYERS; j++) {
                MPPlayer* other = &g_session.players[j];
                if (!other->active || !other->ctx) continue;
                mp_sprites_composite_player(host_fb,
                                            g_session.players[0].ctx,
                                            other->ctx,
                                            other->color_h, other->color_s,
                                            other->color_v);
            }
            return host_fb;
        }

        return raw_fb;
    } else {
        return g_session.client_has_frame ? g_session.client_framebuffer : NULL;
    }
}

bool mp_session_is_active(void) {
    return g_session.state != MP_STATE_IDLE;
}

bool mp_session_is_client_connected(void) {
    return !g_session.is_host && g_session.state == MP_STATE_CONNECTED;
}

/* ---- Settings ---- */

const char* mp_session_get_name(void) {
    return g_session.local_name;
}

void mp_session_set_name(const char* name) {
    strncpy(g_session.local_name, name, MP_MAX_NAME_LEN - 1);
    g_session.local_name[MP_MAX_NAME_LEN - 1] = '\0';
}

void mp_session_get_color(float* h, float* s, float* v) {
    if (h) *h = g_session.local_color_h;
    if (s) *s = g_session.local_color_s;
    if (v) *v = g_session.local_color_v;
}

void mp_session_set_color(float h, float s, float v) {
    g_session.local_color_h = h;
    g_session.local_color_s = s;
    g_session.local_color_v = v;
}

bool mp_session_get_debug_overlay(void) {
    return g_session.debug_overlay;
}

void mp_session_set_debug_overlay(bool visible) {
    g_session.debug_overlay = visible;
}

int mp_session_get_min_hearts(void) {
    return g_session.min_hearts;
}

void mp_session_set_min_hearts(int hearts) {
    if (hearts < 0) hearts = 0;
    if (hearts > 20) hearts = 20;
    g_session.min_hearts = hearts;
}

/* Starting equipment */
void mp_session_set_start_sword(int level) { g_session.start_sword = level; }
void mp_session_set_start_shield(int level) { g_session.start_shield = level; }
void mp_session_set_start_bracelet(int level) { g_session.start_bracelet = level; }
void mp_session_set_start_boots(bool v) { g_session.start_boots = v; }
void mp_session_set_start_flippers(bool v) { g_session.start_flippers = v; }
void mp_session_set_start_feather(bool v) { g_session.start_feather = v; }
void mp_session_set_start_bow(bool v) { g_session.start_bow = v; }
void mp_session_set_start_hookshot(bool v) { g_session.start_hookshot = v; }
void mp_session_set_start_rod(bool v) { g_session.start_rod = v; }
void mp_session_set_start_ocarina(bool v) { g_session.start_ocarina = v; }
void mp_session_set_start_shovel(bool v) { g_session.start_shovel = v; }
void mp_session_set_start_rupees(int v) { g_session.start_rupees = v; }
void mp_session_set_start_bombs(int v) { g_session.start_bombs = v; }
void mp_session_set_start_arrows(int v) { g_session.start_arrows = v; }
void mp_session_set_start_powder(int v) { g_session.start_powder = v; }

int  mp_session_get_start_sword(void) { return g_session.start_sword; }
int  mp_session_get_start_shield(void) { return g_session.start_shield; }
int  mp_session_get_start_bracelet(void) { return g_session.start_bracelet; }
bool mp_session_get_start_boots(void) { return g_session.start_boots; }
bool mp_session_get_start_flippers(void) { return g_session.start_flippers; }
bool mp_session_get_start_feather(void) { return g_session.start_feather; }
bool mp_session_get_start_bow(void) { return g_session.start_bow; }
bool mp_session_get_start_hookshot(void) { return g_session.start_hookshot; }
bool mp_session_get_start_rod(void) { return g_session.start_rod; }
bool mp_session_get_start_ocarina(void) { return g_session.start_ocarina; }
bool mp_session_get_start_shovel(void) { return g_session.start_shovel; }
int  mp_session_get_start_rupees(void) { return g_session.start_rupees; }
int  mp_session_get_start_bombs(void) { return g_session.start_bombs; }
int  mp_session_get_start_arrows(void) { return g_session.start_arrows; }
int  mp_session_get_start_powder(void) { return g_session.start_powder; }
