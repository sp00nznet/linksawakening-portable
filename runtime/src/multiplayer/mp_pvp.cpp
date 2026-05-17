/**
 * @file mp_pvp.cpp
 * @brief PvP Arena Mode implementation
 */

#include "mp_pvp.h"
#include "mp_session.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

extern "C" {
#include "gbrt.h"
}

/* ============================================================================
 * Arena Presets
 * ========================================================================== */

static const MPArenaPreset g_arenas[] = {
    { "Mabe Village Square",    0x92, 0 },
    { "Toronbo Shores",         0xE2, 0 },
    { "Ukuku Prairie",          0x57, 0 },
    { "Tal Tal Heights",        0x07, 0 },
    { "Animal Village",         0xC9, 0 },
    { "Martha's Bay",           0xB2, 0 },
    { "Tail Cave Boss",         0xE5, 1 },
    { "Bottle Grotto Boss",     0xE6, 1 },
    { "Key Cavern Boss",        0xE7, 1 },
    { "Angler's Tunnel Boss",   0xE8, 1 },
    { "Color Dungeon Entry",    0x00, 1 },  /* Color Dungeon rooms */
};

static const int g_arena_count = sizeof(g_arenas) / sizeof(g_arenas[0]);

/* ============================================================================
 * PvP State
 * ========================================================================== */

static struct {
    MPPvPState    state;
    MPPvPConfig   config;
    MPPvPPlayerStats stats[MP_MAX_PLAYERS];
    int           round;
    int           countdown_frames;
    int           round_end_timer;
    int           players_alive;
    int           last_winner;
} g_pvp = { .state = PVP_STATE_OFF };

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

/* ============================================================================
 * Hit Detection
 * ========================================================================== */

static int distance(int x1, int y1, int x2, int y2) {
    int dx = x1 - x2;
    int dy = y1 - y2;
    return (int)sqrtf((float)(dx*dx + dy*dy));
}

static bool is_attacking(GBContext* ctx) {
    uint8_t sword = read_w(ctx, LADX_LINK_SWORD_STATE);
    return sword == SWORD_STATE_SWING || sword == SWORD_STATE_SPIN;
}

static int get_attack_damage(GBContext* ctx) {
    uint8_t sword = read_w(ctx, LADX_LINK_SWORD_STATE);
    if (sword == SWORD_STATE_SPIN) return PVP_SPIN_DAMAGE;
    if (sword == SWORD_STATE_SWING) return PVP_SWORD_DAMAGE;
    return 0;
}

static bool players_in_same_room(GBContext* a, GBContext* b) {
    return read_w(a, 0xD401) == read_w(b, 0xD401) &&
           read_w(a, 0xD402) == read_w(b, 0xD402);
}

/* Teleport a player to the arena */
static void teleport_to_arena(GBContext* ctx, const MPPvPConfig* config, int slot) {
    if (!ctx) return;

    write_w(ctx, 0xD401, config->arena_room);    /* Map room */
    write_w(ctx, 0xD402, config->arena_indoor);   /* Indoor flag */

    /* Spread spawn positions based on slot */
    int spawn_x[] = { 48, 112, 48, 112 };
    int spawn_y[] = { 48,  48, 96,  96 };

    write_w(ctx, 0xD100, spawn_x[slot & 3]);  /* Link X */
    write_w(ctx, 0xD101, spawn_y[slot & 3]);  /* Link Y */

    /* Set starting health */
    uint8_t hp = config->starting_hearts * 8;
    write_w(ctx, 0xDB5A, hp);         /* Health */
    write_w(ctx, 0xDB5B, hp);         /* Max health */
}

/* ============================================================================
 * PvP Update Logic
 * ========================================================================== */

static void check_hits(GBContext** contexts, int count) {
    /* For each attacking player, check if they hit anyone else */
    for (int a = 0; a < count; a++) {
        if (!contexts[a] || !g_pvp.stats[a].alive) continue;
        if (!is_attacking(contexts[a])) continue;

        int ax = read_w(contexts[a], 0xD100);
        int ay = read_w(contexts[a], 0xD101);
        int damage = get_attack_damage(contexts[a]);

        for (int v = 0; v < count; v++) {
            if (v == a || !contexts[v] || !g_pvp.stats[v].alive) continue;
            if (!players_in_same_room(contexts[a], contexts[v])) continue;

            int vx = read_w(contexts[v], 0xD100);
            int vy = read_w(contexts[v], 0xD101);

            int dist = distance(ax, ay, vx, vy);
            if (dist <= PVP_SWORD_RANGE) {
                /* HIT! Apply damage */
                int hp = g_pvp.stats[v].health;
                hp -= damage;
                if (hp < 0) hp = 0;
                g_pvp.stats[v].health = (uint8_t)hp;

                /* Apply to game context */
                write_w(contexts[v], 0xDB5A, (uint8_t)hp);

                /* Apply invincibility frames to prevent multi-hit */
                write_w(contexts[v], 0xDC6F, 60); /* ~1 second invincibility */

                fprintf(stderr, "[PVP] Player %d hit player %d for %d damage (HP: %d)\n",
                        a, v, damage, hp);

                if (hp <= 0) {
                    /* DEATH */
                    g_pvp.stats[v].alive = false;
                    g_pvp.stats[v].deaths++;
                    g_pvp.stats[a].kills++;
                    g_pvp.players_alive--;

                    fprintf(stderr, "[PVP] Player %d eliminated player %d! (%d alive)\n",
                            a, v, g_pvp.players_alive);
                }
            }
        }
    }
}

static void check_round_end(GBContext** contexts, int count) {
    if (g_pvp.players_alive <= 1) {
        /* Find the winner */
        int winner = -1;
        for (int i = 0; i < count; i++) {
            if (g_pvp.stats[i].alive) {
                winner = i;
                break;
            }
        }

        if (winner >= 0) {
            g_pvp.stats[winner].rounds_won++;
            g_pvp.last_winner = winner;

            fprintf(stderr, "[PVP] Round %d winner: Player %d (rounds won: %d/%d)\n",
                    g_pvp.round, winner,
                    g_pvp.stats[winner].rounds_won, g_pvp.config.rounds_to_win);

            /* Check match end */
            if (g_pvp.stats[winner].rounds_won >= g_pvp.config.rounds_to_win) {
                g_pvp.state = PVP_STATE_MATCH_END;
                g_pvp.round_end_timer = 300; /* 5 seconds */
                fprintf(stderr, "[PVP] MATCH WINNER: Player %d!\n", winner);
            } else {
                g_pvp.state = PVP_STATE_ROUND_END;
                g_pvp.round_end_timer = 180; /* 3 seconds */
            }
        } else {
            /* Draw (everyone died simultaneously) */
            g_pvp.state = PVP_STATE_ROUND_END;
            g_pvp.round_end_timer = 180;
            g_pvp.last_winner = -1;
            fprintf(stderr, "[PVP] Round %d: DRAW!\n", g_pvp.round);
        }
    }
}

static void start_round(GBContext** contexts, int count) {
    g_pvp.round++;
    g_pvp.state = PVP_STATE_COUNTDOWN;
    g_pvp.countdown_frames = 180; /* 3 seconds */
    g_pvp.players_alive = 0;

    for (int i = 0; i < count && i < MP_MAX_PLAYERS; i++) {
        if (!contexts[i]) continue;

        g_pvp.stats[i].alive = true;
        g_pvp.stats[i].health = g_pvp.config.starting_hearts * 8;
        g_pvp.stats[i].respawn_timer = 0;
        g_pvp.players_alive++;

        teleport_to_arena(contexts[i], &g_pvp.config, i);
    }

    fprintf(stderr, "[PVP] Round %d starting (%d players)\n",
            g_pvp.round, g_pvp.players_alive);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void mp_pvp_init(void) {
    memset(&g_pvp, 0, sizeof(g_pvp));
    g_pvp.state = PVP_STATE_OFF;
}

MPPvPState mp_pvp_get_state(void) {
    return g_pvp.state;
}

const MPPvPConfig* mp_pvp_get_config(void) {
    return &g_pvp.config;
}

bool mp_pvp_start(const MPPvPConfig* config) {
    if (g_pvp.state != PVP_STATE_OFF) return false;
    if (!config) return false;

    g_pvp.config = *config;
    if (g_pvp.config.starting_hearts == 0) g_pvp.config.starting_hearts = 3;
    if (g_pvp.config.rounds_to_win == 0) g_pvp.config.rounds_to_win = 3;
    if (g_pvp.config.respawn_delay == 0) g_pvp.config.respawn_delay = 120;

    memset(g_pvp.stats, 0, sizeof(g_pvp.stats));
    g_pvp.round = 0;
    g_pvp.state = PVP_STATE_LOBBY;

    fprintf(stderr, "[PVP] Match started! Arena: room 0x%02X, %d hearts, best of %d\n",
            config->arena_room, config->starting_hearts, config->rounds_to_win * 2 - 1);
    return true;
}

void mp_pvp_stop(void) {
    g_pvp.state = PVP_STATE_OFF;
    fprintf(stderr, "[PVP] Match stopped\n");
}

void mp_pvp_update(GBContext** contexts, int count) {
    if (g_pvp.state == PVP_STATE_OFF) return;

    switch (g_pvp.state) {
    case PVP_STATE_LOBBY:
        /* Wait for host to trigger round start */
        /* Auto-start if we have 2+ players */
        if (count >= 2) {
            start_round(contexts, count);
        }
        break;

    case PVP_STATE_COUNTDOWN:
        g_pvp.countdown_frames--;
        if (g_pvp.countdown_frames <= 0) {
            g_pvp.state = PVP_STATE_FIGHTING;
            fprintf(stderr, "[PVP] FIGHT!\n");
        }
        break;

    case PVP_STATE_FIGHTING:
        /* Check for hits */
        check_hits(contexts, count);

        /* Enforce arena boundaries - keep players in the arena room */
        for (int i = 0; i < count && i < MP_MAX_PLAYERS; i++) {
            if (!contexts[i] || !g_pvp.stats[i].alive) continue;

            uint8_t room = read_w(contexts[i], 0xD401);
            uint8_t indoor = read_w(contexts[i], 0xD402);

            /* If a player left the arena, teleport them back */
            if (room != g_pvp.config.arena_room ||
                indoor != g_pvp.config.arena_indoor) {
                teleport_to_arena(contexts[i], &g_pvp.config, i);
            }

            /* Keep invincibility counter in sync */
            g_pvp.stats[i].health = read_w(contexts[i], 0xDB5A);
        }

        /* Check for round end */
        check_round_end(contexts, count);
        break;

    case PVP_STATE_ROUND_END:
        g_pvp.round_end_timer--;
        if (g_pvp.round_end_timer <= 0) {
            start_round(contexts, count);
        }
        break;

    case PVP_STATE_MATCH_END:
        g_pvp.round_end_timer--;
        if (g_pvp.round_end_timer <= 0) {
            g_pvp.state = PVP_STATE_OFF;
            fprintf(stderr, "[PVP] Match complete!\n");
        }
        break;

    default:
        break;
    }
}

const MPPvPPlayerStats* mp_pvp_get_stats(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS) return NULL;
    return &g_pvp.stats[slot];
}

int mp_pvp_get_round(void) {
    return g_pvp.round;
}

const MPArenaPreset* mp_pvp_get_arenas(int* count) {
    if (count) *count = g_arena_count;
    return g_arenas;
}

void mp_pvp_handle_packet(const void* data, uint32_t size) {
    if (!data || size < sizeof(MPPacketHeader)) return;

    const MPPacketHeader* hdr = (const MPPacketHeader*)data;

    switch (hdr->type) {
    case MP_PKT_PVP_START: {
        if (size < sizeof(MPPvPStartPacket)) return;
        const MPPvPStartPacket* pkt = (const MPPvPStartPacket*)data;
        mp_pvp_start(&pkt->config);
        break;
    }

    case MP_PKT_PVP_HIT: {
        if (size < sizeof(MPPvPHitPacket)) return;
        const MPPvPHitPacket* hit = (const MPPvPHitPacket*)data;
        /* Client-side: update local stats display */
        if (hit->victim_slot < MP_MAX_PLAYERS) {
            int hp = g_pvp.stats[hit->victim_slot].health;
            hp -= hit->damage;
            if (hp < 0) hp = 0;
            g_pvp.stats[hit->victim_slot].health = (uint8_t)hp;
        }
        break;
    }

    case MP_PKT_PVP_ROUND: {
        if (size < sizeof(MPPvPRoundPacket)) return;
        const MPPvPRoundPacket* rnd = (const MPPvPRoundPacket*)data;
        g_pvp.round = rnd->round_number;
        g_pvp.last_winner = rnd->winner_slot;
        memcpy(g_pvp.stats, rnd->stats, sizeof(g_pvp.stats));
        break;
    }

    default:
        break;
    }
}
