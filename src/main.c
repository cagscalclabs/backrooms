#include <graphx.h>
#include <keypadc.h>
#include <fileioc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gfx/gfx.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define RAY_COLS_HIGH 52
#define RAY_COLS_LOW 48
#define TEX_SIZE 8
#define TEX_H 32
#define VIEW_TOP 12
#define VIEW_BOTTOM (SCREEN_H - 13)
#define DDA_STEP_CAP 20
#define RECIP_LUT_SIZE 8192
#define ACTION_QUEUE_SIZE 16
#define ACTION_BUDGET 4

#define MAP_W 24
#define MAP_H 24
#define LEVEL_COUNT 4
#define ENTITY_MAX 20

#define FIX_SHIFT 16
#define FIX_ONE (1 << FIX_SHIFT)
#define ANGLE_STEPS 256
#define TRIG_SHIFT 12
#define TRIG_ONE (1 << TRIG_SHIFT)

#define MOVE_SPEED 2240       /* faster baseline traversal */
#define STRAFE_SPEED 1880
#define RUN_MULT_NUM 2
#define RUN_MULT_DEN 1
#define TURN_SPEED 7
#define SPRITE_REF_H 32

#define TILE_EMPTY 0
#define TILE_WALL 1
#define TILE_EXIT 3
#define TILE_WARP 4
#define TILE_CONSOLE 5

#define ENTITY_WATCHER 0
#define ENTITY_FRAGMENT 1
#define ENTITY_WHISPER 2
#define ENTITY_TONIC 3
#define ENTITY_ANTI 4

#define MSG_TIME 120
#define SANITY_MAX 100
#define SANITY_DECAY_FRAMES 360
#define SANITY_CONTACT_COOLDOWN 40
#define SANITY_HIT_NORMAL 3
#define SANITY_HIT_PANIC 8
#define SHARD_SIGHT 0
#define SHARD_MOVEMENT 1
#define SHARD_TIME 2
#define SHARD_MIND 3
#define FX_SIGHT (1u << 0)
#define FX_MOVEMENT (1u << 1)
#define FX_TIME (1u << 2)
#define FX_MIND (1u << 3)
#define AFX_SIGHT (1u << 0)
#define AFX_MOVEMENT (1u << 1)
#define AFX_TIME (1u << 2)
#define AFX_MIND (1u << 3)
#define HISTORY_LEN 128
#define SAVE_NAME "BKSAVE"
#define SAVE_VER 1

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t type;
    uint8_t level;
    uint8_t alive;
} entity_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t angle;
    uint8_t level;
} player_t;

typedef struct {
    uint8_t ver;
    uint8_t sanity;
    uint8_t shard_bits;
    uint8_t effect_flags;
    uint8_t anti_bits;
    uint8_t anti_effect_flags;
    uint8_t reserved;
} save_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t angle;
    uint8_t level;
} history_t;

typedef enum {
    ACT_TICK = 0,
    ACT_INTERACT,
    ACT_RENDER_WORLD,
    ACT_RENDER_ENTITIES,
    ACT_RENDER_OVERLAY,
    ACT_RENDER_HUD,
    ACT_SWAP
} action_t;

static int16_t s_sin[ANGLE_STEPS];
static int16_t s_cos[ANGLE_STEPS];
static uint16_t s_zbuf[RAY_COLS_HIGH];
static uint16_t s_col_edges_high[RAY_COLS_HIGH + 1];
static uint16_t s_col_edges_low[RAY_COLS_LOW + 1];
static int16_t s_camx_high[RAY_COLS_HIGH];
static int16_t s_camx_low[RAY_COLS_LOW];
static uint8_t s_x_to_ray_high[SCREEN_W];
static uint8_t s_x_to_ray_low[SCREEN_W];
static uint32_t s_recip_lut[RECIP_LUT_SIZE];

static entity_t s_entities[ENTITY_MAX];
static uint8_t s_entity_count;

static player_t s_player;
static uint8_t s_fragments;
static uint8_t s_console_bits;
static uint8_t s_msg_timer;
static char s_msg[30];
static uint8_t s_seed = 7;
static uint16_t s_frame = 0;
static uint8_t s_sanity = SANITY_MAX;
static uint16_t s_sanity_decay_tick = 0;
static uint8_t s_contact_cooldown = 0;
static bool s_defeat = false;
static uint8_t s_cached_stage = 0xFFu;
static uint8_t s_shard_bits = 0;
static uint8_t s_effect_flags = 0;
static uint8_t s_anti_bits = 0;
static uint8_t s_anti_flags = 0;
static uint16_t s_invert_next = 0;
static uint8_t s_invert_active = 0;
static uint16_t s_time_next = 0;
static uint8_t s_sight_dark_ticks = 0;
static uint16_t s_sight_dark_cooldown = 180;
static history_t s_history[HISTORY_LEN];
static uint8_t s_hist_head = 0;
static uint8_t s_hist_fill = 0;
static uint16_t s_flavor_delay = 90;
static uint8_t s_active_cols = RAY_COLS_HIGH;
static bool s_fast_render = false;
static bool s_run_held = false;
static bool s_turning = false;
static bool s_moved_this_tick = false;
static bool s_turned_this_tick = false;
static action_t s_action_queue[ACTION_QUEUE_SIZE];
static uint8_t s_action_head = 0;
static uint8_t s_action_tail = 0;
static uint8_t s_action_count = 0;
static uint8_t s_map[LEVEL_COUNT][MAP_H][MAP_W];
static const uint8_t s_wall_tex[LEVEL_COUNT][TEX_SIZE][TEX_SIZE] = {
    {
        {0xE7,0xE7,0xE7,0xE6,0xE7,0xE7,0xE7,0xE6},
        {0xE7,0xE7,0xE7,0xE6,0xE7,0xE7,0xE7,0xE6},
        {0xE7,0xE7,0xE6,0xE7,0xE7,0xE7,0xE6,0xE7},
        {0xE7,0xE7,0xE6,0xE7,0xE7,0xE7,0xE6,0xE7},
        {0xE7,0xE6,0xE7,0xE7,0xE7,0xE6,0xE7,0xE7},
        {0xE7,0xE6,0xE7,0xE7,0xE7,0xE6,0xE7,0xE7},
        {0xE6,0xE7,0xE7,0xE7,0xE6,0xE7,0xE7,0xE7},
        {0xE6,0xE7,0xE7,0xE7,0xE6,0xE7,0xE7,0xE7}
    },
    {
        {242,242,242,241,242,242,242,241},
        {242,242,242,241,242,242,242,241},
        {243,243,243,242,243,243,243,242},
        {243,243,243,242,243,243,243,242},
        {242,242,242,241,242,242,242,241},
        {242,242,242,241,242,242,242,241},
        {241,241,241,240,241,241,241,240},
        {241,241,241,240,241,241,241,240}
    },
    {
        {139,139,139,138,139,139,139,138},
        {139,139,139,138,139,139,139,138},
        {138,138,138,137,138,138,138,137},
        {138,138,138,137,138,138,138,137},
        {137,137,137,136,137,137,137,136},
        {137,137,137,136,137,137,137,136},
        {136,136,136,135,136,136,136,135},
        {136,136,136,135,136,136,136,135}
    },
    {
        {247,247,247,246,247,247,247,246},
        {247,247,247,246,247,247,247,246},
        {246,246,246,245,246,246,246,245},
        {246,246,246,245,246,246,246,245},
        {245,245,245,244,245,245,245,244},
        {245,245,245,244,245,245,245,244},
        {244,244,244,243,244,244,244,243},
        {244,244,244,243,244,244,244,243}
    }
};
static const uint8_t s_ceil_tex[LEVEL_COUNT][TEX_SIZE][TEX_SIZE] = {
    {
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95},
        {0x95,0x95,0x95,0x95,0x95,0x95,0x95,0x95}
    },
    {
        {244,245,244,245,244,245,244,245},
        {243,244,243,244,243,244,243,244},
        {242,243,242,243,242,243,242,243},
        {243,244,243,244,243,244,243,244},
        {244,245,244,245,244,245,244,245},
        {243,244,243,244,243,244,243,244},
        {242,243,242,243,242,243,242,243},
        {243,244,243,244,243,244,243,244}
    },
    {
        {150,151,150,151,150,151,150,151},
        {149,150,149,150,149,150,149,150},
        {148,149,148,149,148,149,148,149},
        {149,150,149,150,149,150,149,150},
        {150,151,150,151,150,151,150,151},
        {149,150,149,150,149,150,149,150},
        {148,149,148,149,148,149,148,149},
        {149,150,149,150,149,150,149,150}
    },
    {
        {247,248,247,248,247,248,247,248},
        {246,247,246,247,246,247,246,247},
        {245,246,245,246,245,246,245,246},
        {246,247,246,247,246,247,246,247},
        {247,248,247,248,247,248,247,248},
        {246,247,246,247,246,247,246,247},
        {245,246,245,246,245,246,245,246},
        {246,247,246,247,246,247,246,247}
    }
};
static const uint8_t s_floor_tex[LEVEL_COUNT][TEX_SIZE][TEX_SIZE] = {
    {
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5},
        {0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5,0xC5}
    },
    {
        {236,237,236,237,236,237,236,237},
        {235,236,235,236,235,236,235,236},
        {234,235,234,235,234,235,234,235},
        {235,236,235,236,235,236,235,236},
        {236,237,236,237,236,237,236,237},
        {235,236,235,236,235,236,235,236},
        {234,235,234,235,234,235,234,235},
        {235,236,235,236,235,236,235,236}
    },
    {
        {132,133,132,133,132,133,132,133},
        {131,132,131,132,131,132,131,132},
        {130,131,130,131,130,131,130,131},
        {131,132,131,132,131,132,131,132},
        {132,133,132,133,132,133,132,133},
        {131,132,131,132,131,132,131,132},
        {130,131,130,131,130,131,130,131},
        {131,132,131,132,131,132,131,132}
    },
    {
        {241,242,241,242,241,242,241,242},
        {240,241,240,241,240,241,240,241},
        {239,240,239,240,239,240,239,240},
        {240,241,240,241,240,241,240,241},
        {241,242,241,242,241,242,241,242},
        {240,241,240,241,240,241,240,241},
        {239,240,239,240,239,240,239,240},
        {240,241,240,241,240,241,240,241}
    }
};
static uint8_t s_wall_tex_cached[LEVEL_COUNT][TEX_SIZE][TEX_SIZE];
static uint8_t s_ceil_tex_cached[LEVEL_COUNT][TEX_SIZE][TEX_SIZE];
static uint8_t s_floor_tex_cached[LEVEL_COUNT][TEX_SIZE][TEX_SIZE];
static const uint8_t s_shard_glitch[8] = {
    0xF7, 0xEF, 0xE6, 0xCE, 0x7D, 0x5F, 0x3F, 0x9F
};
static const uint8_t s_warp_x[3] = {22, 22, 22};
static const uint8_t s_warp_y[3] = {2, 2, 2};
static const uint8_t s_shard_kind_by_level[3] = {SHARD_SIGHT, SHARD_MOVEMENT, SHARD_TIME};
static const uint8_t s_anti_kind_by_level[3] = {SHARD_MOVEMENT, SHARD_TIME, SHARD_SIGHT};

static bool queue_push(action_t a) {
    if (s_action_count >= ACTION_QUEUE_SIZE) {
        return false;
    }
    s_action_queue[s_action_tail] = a;
    s_action_tail = (uint8_t)((s_action_tail + 1u) % ACTION_QUEUE_SIZE);
    s_action_count++;
    return true;
}

static bool queue_pop(action_t *out) {
    if (s_action_count == 0u) {
        return false;
    }
    *out = s_action_queue[s_action_head];
    s_action_head = (uint8_t)((s_action_head + 1u) % ACTION_QUEUE_SIZE);
    s_action_count--;
    return true;
}

static void build_level(uint8_t lvl) {
    uint8_t x;
    uint8_t y;

    for (y = 0; y < MAP_H; y++) {
        for (x = 0; x < MAP_W; x++) {
            s_map[lvl][y][x] = (x == 0 || y == 0 || x == (MAP_W - 1) || y == (MAP_H - 1)) ? TILE_WALL : TILE_EMPTY;
        }
    }
}

static void init_maps(void) {
    uint8_t x;
    uint8_t y;

    for (x = 0; x < LEVEL_COUNT; x++) {
        build_level(x);
    }

    /* Level 0: yellow maze feel with staggered columns. */
    for (y = 2; y < MAP_H - 2; y += 3) {
        for (x = 2; x < MAP_W - 2; x++) {
            if (((x + y) & 1u) == 0u) {
                s_map[0][y][x] = TILE_WALL;
            }
        }
    }
    for (y = 3; y < MAP_H - 2; y++) {
        if ((y & 3u) != 0u) {
            s_map[0][y][8] = TILE_WALL;
            s_map[0][y][15] = TILE_WALL;
        }
    }
    for (x = 1; x < MAP_W - 1; x++) {
        s_map[0][1][x] = TILE_EMPTY;
    }
    s_map[0][20][4] = TILE_CONSOLE;
    s_map[0][2][22] = TILE_WARP;

    /* Level 1: stark grid with long lanes. */
    for (x = 3; x < MAP_W - 3; x += 4) {
        for (y = 2; y < MAP_H - 2; y++) {
            if ((y % 5u) != 1u) {
                s_map[1][y][x] = TILE_WALL;
            }
        }
    }
    for (y = 4; y < MAP_H - 3; y += 4) {
        for (x = 2; x < MAP_W - 2; x++) {
            if ((x % 6u) != 0u) {
                s_map[1][y][x] = TILE_WALL;
            }
        }
    }
    for (x = 1; x < MAP_W - 1; x++) {
        s_map[1][1][x] = TILE_EMPTY;
    }
    s_map[1][18][20] = TILE_CONSOLE;
    s_map[1][2][22] = TILE_WARP;

    /* Level 2: chase-style corridors with return loops. */
    for (y = 2; y < MAP_H - 2; y++) {
        for (x = 2; x < MAP_W - 2; x++) {
            if ((x % 2u) == 0u) {
                s_map[2][y][x] = TILE_WALL;
            }
        }
    }
    for (y = 2; y < MAP_H - 2; y++) {
        s_map[2][y][2] = TILE_EMPTY;
        s_map[2][y][11] = TILE_EMPTY;
        s_map[2][y][20] = TILE_EMPTY;
    }
    for (x = 2; x < MAP_W - 2; x++) {
        if ((x & 3u) != 0u) {
            s_map[2][5][x] = TILE_EMPTY;
            s_map[2][12][x] = TILE_EMPTY;
            s_map[2][19][x] = TILE_EMPTY;
        }
    }
    s_map[2][20][3] = TILE_CONSOLE;
    s_map[2][2][22] = TILE_EXIT;

    /* Level 3: calm open void. */
    for (y = 1; y < MAP_H - 1; y++) {
        for (x = 1; x < MAP_W - 1; x++) {
            s_map[3][y][x] = TILE_EMPTY;
        }
    }

    /* Keep spawn and immediate movement lanes clear on all playable levels. */
    for (x = 0; x < 3; x++) {
        s_map[0][2][(uint8_t)(2 + x)] = TILE_EMPTY;
        s_map[1][2][(uint8_t)(2 + x)] = TILE_EMPTY;
        s_map[2][2][(uint8_t)(2 + x)] = TILE_EMPTY;
    }
    for (y = 0; y < 3; y++) {
        s_map[0][(uint8_t)(2 + y)][2] = TILE_EMPTY;
        s_map[1][(uint8_t)(2 + y)][2] = TILE_EMPTY;
        s_map[2][(uint8_t)(2 + y)][2] = TILE_EMPTY;
    }
}

static void reveal_warp_for_level(uint8_t lvl) {
    if (lvl < 2u) {
        s_map[lvl][s_warp_y[lvl]][s_warp_x[lvl]] = TILE_WARP;
    } else if (lvl == 2u) {
        s_map[2][s_warp_y[2]][s_warp_x[2]] = TILE_EXIT;
    }
}

static void apply_shard_progress(void) {
    uint8_t lvl;
    for (lvl = 0; lvl < 3u; lvl++) {
        if (s_shard_bits & (1u << lvl)) {
            reveal_warp_for_level(lvl);
        }
    }
}

static uint8_t map_at(uint8_t lvl, int16_t x, int16_t y) {
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) {
        return TILE_WALL;
    }
    return s_map[lvl][(uint8_t)y][(uint8_t)x];
}

static void set_msg(const char *t) {
    strncpy(s_msg, t, sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = '\0';
    s_msg_timer = MSG_TIME;
}

static void init_trig(void) {
    int32_t x = TRIG_ONE;
    int32_t y = 0;
    const int32_t c = 4095;
    const int32_t s = 101;
    uint16_t i;

    for (i = 0; i < ANGLE_STEPS; i++) {
        s_cos[i] = (int16_t)x;
        s_sin[i] = (int16_t)y;
        {
            int32_t nx = ((x * c) - (y * s)) >> TRIG_SHIFT;
            int32_t ny = ((y * c) + (x * s)) >> TRIG_SHIFT;
            x = nx;
            y = ny;
        }
    }
}

static void init_render_tables(void) {
    uint8_t i;
    uint16_t r;
    int x;
    for (i = 0; i <= RAY_COLS_HIGH; i++) {
        s_col_edges_high[i] = (uint16_t)(((int32_t)i * SCREEN_W) / RAY_COLS_HIGH);
    }
    for (i = 0; i <= RAY_COLS_LOW; i++) {
        s_col_edges_low[i] = (uint16_t)(((int32_t)i * SCREEN_W) / RAY_COLS_LOW);
    }
    for (i = 0; i < RAY_COLS_HIGH; i++) {
        s_camx_high[i] = (int16_t)((((int32_t)i * 2 * TRIG_ONE) / RAY_COLS_HIGH) - TRIG_ONE);
    }
    for (i = 0; i < RAY_COLS_LOW; i++) {
        s_camx_low[i] = (int16_t)((((int32_t)i * 2 * TRIG_ONE) / RAY_COLS_LOW) - TRIG_ONE);
    }
    for (x = 0; x < SCREEN_W; x++) {
        int idx_h = (x * RAY_COLS_HIGH) / SCREEN_W;
        int idx_l = (x * RAY_COLS_LOW) / SCREEN_W;
        if (idx_h >= RAY_COLS_HIGH) {
            idx_h = RAY_COLS_HIGH - 1;
        }
        if (idx_l >= RAY_COLS_LOW) {
            idx_l = RAY_COLS_LOW - 1;
        }
        s_x_to_ray_high[x] = (uint8_t)idx_h;
        s_x_to_ray_low[x] = (uint8_t)idx_l;
    }

    s_recip_lut[0] = 0x3FFFFFFFu;
    for (r = 1; r < RECIP_LUT_SIZE; r++) {
        s_recip_lut[r] = (uint32_t)(((int64_t)1 << 28) / r);
    }
}

static uint8_t prng8(void) {
    s_seed = (uint8_t)(s_seed * 73u + 29u);
    return s_seed;
}

static uint8_t sanity_stage(void) {
    if (s_sanity <= 25u) {
        return 3u;
    }
    if (s_sanity <= 50u) {
        return 2u;
    }
    if (s_sanity <= 75u) {
        return 1u;
    }
    return 0u;
}

static void set_flavor_hint(void) {
    if (s_player.level == 0u) {
        set_msg("one wall stripe breaks pattern");
    } else if (s_player.level == 1u) {
        set_msg("one seam is out of phase");
    } else if (s_player.level == 2u) {
        set_msg("one bend should not exist");
    } else {
        set_msg("the calm is suspicious");
    }
}

static void tick_flavor_hint(void) {
    if (s_defeat || s_msg_timer) {
        return;
    }
    if (s_flavor_delay > 0u) {
        s_flavor_delay--;
        return;
    }
    set_flavor_hint();
    s_flavor_delay = 420u;
}

static void apply_shard_effect(uint8_t kind) {
    if (kind == SHARD_SIGHT) {
        s_effect_flags |= FX_SIGHT;
        set_msg("sight fractures; not all forms are real");
    } else if (kind == SHARD_MOVEMENT) {
        s_effect_flags |= FX_MOVEMENT;
        s_invert_next = 120u;
        set_msg("movement desyncs in sudden pulses");
    } else if (kind == SHARD_TIME) {
        s_effect_flags |= FX_TIME;
        s_time_next = 180u;
        set_msg("time slips and pulls you backward");
    } else if (kind == SHARD_MIND) {
        s_effect_flags |= FX_MIND;
        set_msg("mind frays; sanity drains faster");
    }
}

static void apply_anti_effect(uint8_t kind) {
    if (kind == SHARD_SIGHT) {
        s_anti_flags |= AFX_SIGHT;
        set_msg("sight anchors to subtle traces");
    } else if (kind == SHARD_MOVEMENT) {
        s_anti_flags |= AFX_MOVEMENT;
        set_msg("movement resists sudden inversion");
    } else if (kind == SHARD_TIME) {
        s_anti_flags |= AFX_TIME;
        set_msg("time grip strengthens");
    } else if (kind == SHARD_MIND) {
        s_anti_flags |= AFX_MIND;
        set_msg("mind steadies against corruption");
    }
}

static void sync_effects_from_shards(void) {
    uint8_t lvl;
    if (s_effect_flags == 0u) {
        for (lvl = 0; lvl < 3u; lvl++) {
            if (s_shard_bits & (1u << lvl)) {
                uint8_t k = s_shard_kind_by_level[lvl];
                if (k == SHARD_SIGHT) s_effect_flags |= FX_SIGHT;
                if (k == SHARD_MOVEMENT) s_effect_flags |= FX_MOVEMENT;
                if (k == SHARD_TIME) s_effect_flags |= FX_TIME;
            }
        }
        if ((s_shard_bits & 0x07u) == 0x07u) {
            s_effect_flags |= FX_MIND;
        }
    }
    if ((s_effect_flags & FX_MOVEMENT) && s_invert_next == 0u) {
        s_invert_next = 120u;
    }
    if ((s_effect_flags & FX_TIME) && s_time_next == 0u) {
        s_time_next = 180u;
    }
}

static void sync_anti_effects_from_bits(void) {
    uint8_t lvl;
    if (s_anti_flags == 0u) {
        for (lvl = 0; lvl < 3u; lvl++) {
            if (s_anti_bits & (1u << lvl)) {
                uint8_t k = s_anti_kind_by_level[lvl];
                if (k == SHARD_SIGHT) s_anti_flags |= AFX_SIGHT;
                if (k == SHARD_MOVEMENT) s_anti_flags |= AFX_MOVEMENT;
                if (k == SHARD_TIME) s_anti_flags |= AFX_TIME;
            }
        }
        if ((s_anti_bits & 0x07u) == 0x07u) {
            s_anti_flags |= AFX_MIND;
        }
    }
}

static void tick_time_history(void) {
    s_history[s_hist_head].x = s_player.x;
    s_history[s_hist_head].y = s_player.y;
    s_history[s_hist_head].angle = s_player.angle;
    s_history[s_hist_head].level = s_player.level;
    s_hist_head = (uint8_t)((s_hist_head + 1u) % HISTORY_LEN);
    if (s_hist_fill < HISTORY_LEN) {
        s_hist_fill++;
    }
}

static uint8_t mix_corrupt_color_stage(uint8_t base, uint8_t alt_a, uint8_t alt_b, int x, int y, uint8_t stage) {
    uint8_t p = (uint8_t)((x + (y << 1)) & 15u);
    if (stage == 0u) {
        return base;
    }
    if (stage == 1u) {
        if (p == 0u) return alt_a;
        if (p == 7u) return alt_b;
        return base;
    }
    if (stage == 2u) {
        if (p < 3u) return alt_a;
        if (p < 6u) return alt_b;
        return base;
    }
    if (p < 6u) return alt_a;
    if (p < 11u) return alt_b;
    return base;
}

static void rebuild_corruption_cache(uint8_t stage) {
    uint8_t lvl;
    uint8_t y;
    uint8_t x;

    for (lvl = 0; lvl < LEVEL_COUNT; lvl++) {
        uint8_t ceil_base = s_ceil_tex[lvl][3][3];
        uint8_t floor_base = s_floor_tex[lvl][4][4];
        uint8_t wall_base = s_wall_tex[lvl][3][3];
        for (y = 0; y < TEX_SIZE; y++) {
            for (x = 0; x < TEX_SIZE; x++) {
                uint8_t w = s_wall_tex[lvl][y][x];
                uint8_t c = s_ceil_tex[lvl][y][x];
                uint8_t f = s_floor_tex[lvl][y][x];
                s_wall_tex_cached[lvl][y][x] = mix_corrupt_color_stage(w, ceil_base, floor_base, x, y, stage);
                s_ceil_tex_cached[lvl][y][x] = mix_corrupt_color_stage(c, wall_base, floor_base, x + 3, y + 5, stage);
                s_floor_tex_cached[lvl][y][x] = mix_corrupt_color_stage(f, wall_base, ceil_base, x + 7, y + 1, stage);
            }
        }
    }
    s_cached_stage = stage;
}

static void save_progress(void) {
    ti_var_t f;
    save_t sv;
    sv.ver = SAVE_VER;
    sv.sanity = s_sanity;
    sv.shard_bits = s_shard_bits;
    sv.effect_flags = s_effect_flags;
    sv.anti_bits = s_anti_bits;
    sv.anti_effect_flags = s_anti_flags;
    sv.reserved = (s_player.level < LEVEL_COUNT) ? s_player.level : 0u;

    f = ti_Open(SAVE_NAME, "w");
    if (f) {
        ti_Write(&sv, sizeof(save_t), 1, f);
        ti_Close(f);
    }
}

static void load_progress(void) {
    ti_var_t f;
    save_t sv;
    memset(&sv, 0, sizeof(save_t));
    f = ti_Open(SAVE_NAME, "r");
    if (!f) {
        return;
    }
    if (ti_Read(&sv, sizeof(save_t), 1, f) == 1 && sv.ver == SAVE_VER) {
        s_sanity = (sv.sanity <= SANITY_MAX) ? sv.sanity : SANITY_MAX;
        s_shard_bits = (uint8_t)(sv.shard_bits & 0x07u);
        s_effect_flags = sv.effect_flags;
        s_anti_bits = (uint8_t)(sv.anti_bits & 0x07u);
        s_anti_flags = sv.anti_effect_flags;
        s_player.level = (sv.reserved < LEVEL_COUNT) ? sv.reserved : 0u;
    }
    ti_Close(f);
}

static bool tile_blocks(uint8_t t) {
    return t == TILE_WALL || t == TILE_EXIT || t == TILE_CONSOLE;
}

static void move_player(int32_t dx, int32_t dy) {
    int32_t nx = s_player.x + dx;
    int32_t ny = s_player.y + dy;
    int16_t tx = (int16_t)(nx >> FIX_SHIFT);
    int16_t ty = (int16_t)(s_player.y >> FIX_SHIFT);

    if (!tile_blocks(map_at(s_player.level, tx, ty))) {
        s_player.x = nx;
        s_moved_this_tick = true;
    }

    tx = (int16_t)(s_player.x >> FIX_SHIFT);
    ty = (int16_t)(ny >> FIX_SHIFT);
    if (!tile_blocks(map_at(s_player.level, tx, ty))) {
        s_player.y = ny;
        s_moved_this_tick = true;
    }
}

static void spawn_entities(void) {
    s_entity_count = 0;

    s_entities[s_entity_count++] = (entity_t){ (18 << FIX_SHIFT) + (FIX_ONE / 2), (3 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_FRAGMENT, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (19 << FIX_SHIFT) + (FIX_ONE / 2), (18 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_FRAGMENT, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (4 << FIX_SHIFT) + (FIX_ONE / 2), (20 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_FRAGMENT, 2, 1 };

    s_entities[s_entity_count++] = (entity_t){ (6 << FIX_SHIFT) + (FIX_ONE / 2), (3 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WATCHER, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (12 << FIX_SHIFT) + (FIX_ONE / 2), (13 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WATCHER, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (20 << FIX_SHIFT) + (FIX_ONE / 2), (10 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WATCHER, 2, 1 };

    s_entities[s_entity_count++] = (entity_t){ (3 << FIX_SHIFT) + (FIX_ONE / 2), (17 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WHISPER, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (6 << FIX_SHIFT) + (FIX_ONE / 2), (19 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WHISPER, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (8 << FIX_SHIFT) + (FIX_ONE / 2), (21 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_WHISPER, 2, 1 };

    s_entities[s_entity_count++] = (entity_t){ (5 << FIX_SHIFT) + (FIX_ONE / 2), (6 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (17 << FIX_SHIFT) + (FIX_ONE / 2), (10 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (10 << FIX_SHIFT) + (FIX_ONE / 2), (6 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (18 << FIX_SHIFT) + (FIX_ONE / 2), (15 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (3 << FIX_SHIFT) + (FIX_ONE / 2), (10 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 2, 1 };
    s_entities[s_entity_count++] = (entity_t){ (20 << FIX_SHIFT) + (FIX_ONE / 2), (18 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_TONIC, 2, 1 };

    s_entities[s_entity_count++] = (entity_t){ (21 << FIX_SHIFT) + (FIX_ONE / 2), (6 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_ANTI, 0, 1 };
    s_entities[s_entity_count++] = (entity_t){ (21 << FIX_SHIFT) + (FIX_ONE / 2), (14 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_ANTI, 1, 1 };
    s_entities[s_entity_count++] = (entity_t){ (7 << FIX_SHIFT) + (FIX_ONE / 2), (18 << FIX_SHIFT) + (FIX_ONE / 2), ENTITY_ANTI, 2, 1 };
}

static void apply_entity_progress(void) {
    uint8_t i;
    s_fragments = 0;
    for (i = 0; i < s_entity_count; i++) {
        entity_t *e = &s_entities[i];
        if (e->type == ENTITY_FRAGMENT && (s_shard_bits & (1u << e->level))) {
            e->alive = 0;
            s_fragments++;
        } else if (e->type == ENTITY_ANTI && (s_anti_bits & (1u << e->level))) {
            e->alive = 0;
        }
    }
}

static void warp_to_level(uint8_t lvl) {
    s_player.level = lvl;
    s_player.x = (2 << FIX_SHIFT) + (FIX_ONE / 2);
    s_player.y = (2 << FIX_SHIFT) + (FIX_ONE / 2);
    s_player.angle = 0;
}

static void check_pickups(void) {
    uint8_t i;
    for (i = 0; i < s_entity_count; i++) {
        entity_t *e = &s_entities[i];
        int32_t dx;
        int32_t dy;
        int64_t d2;

        if (!e->alive || e->level != s_player.level) {
            continue;
        }

        dx = e->x - s_player.x;
        dy = e->y - s_player.y;
        d2 = (int64_t)dx * dx + (int64_t)dy * dy;
        if (e->type == ENTITY_FRAGMENT && d2 < ((int64_t)FIX_ONE * FIX_ONE) / 3) {
            uint8_t shard_kind = s_shard_kind_by_level[s_player.level];
            e->alive = 0;
            s_fragments++;
            s_shard_bits |= (uint8_t)(1u << s_player.level);
            reveal_warp_for_level(s_player.level);
            apply_shard_effect(shard_kind);
            if ((s_shard_bits & 0x07u) == 0x07u) {
                apply_shard_effect(SHARD_MIND);
            }
            save_progress();
        } else if (e->type == ENTITY_TONIC && d2 < ((int64_t)FIX_ONE * FIX_ONE) / 2) {
            e->alive = 0;
            if (s_sanity <= 79u) {
                s_sanity = (uint8_t)(s_sanity + 20u);
            } else {
                s_sanity = SANITY_MAX;
            }
            set_msg("your head clears a little");
            s_flavor_delay = 120u;
            save_progress();
        } else if (e->type == ENTITY_ANTI && d2 < ((int64_t)FIX_ONE * FIX_ONE) / 3) {
            uint8_t anti_kind = s_anti_kind_by_level[s_player.level];
            e->alive = 0;
            s_anti_bits |= (uint8_t)(1u << s_player.level);
            apply_anti_effect(anti_kind);
            if ((s_anti_bits & 0x07u) == 0x07u) {
                apply_anti_effect(SHARD_MIND);
            }
            save_progress();
        }
    }
}

static void interact(void) {
    int32_t fx = s_cos[s_player.angle];
    int32_t fy = s_sin[s_player.angle];
    int32_t tx = s_player.x + ((fx * (FIX_ONE / 2)) >> TRIG_SHIFT);
    int32_t ty = s_player.y + ((fy * (FIX_ONE / 2)) >> TRIG_SHIFT);
    int16_t mx = (int16_t)(tx >> FIX_SHIFT);
    int16_t my = (int16_t)(ty >> FIX_SHIFT);
    uint8_t t = map_at(s_player.level, mx, my);

    if (t == TILE_CONSOLE) {
        uint8_t bit = (uint8_t)(1u << s_player.level);
        if ((s_console_bits & bit) == 0) {
            s_console_bits |= bit;
            if (s_sanity <= 89u) {
                s_sanity = (uint8_t)(s_sanity + 10u);
            } else {
                s_sanity = SANITY_MAX;
            }
            set_msg("the console steadies your breathing");
            s_flavor_delay = 120u;
            save_progress();
        } else {
            set_msg("the panel stays unchanged");
        }
        return;
    }

    if (t == TILE_WARP) {
        uint8_t next = (uint8_t)(s_player.level + 1u);
        if (next > 2u) {
            set_msg("no path forward here");
            return;
        }
        if ((s_shard_bits & (1u << s_player.level)) == 0u) {
            set_msg("the warp is inert; find the shard");
            return;
        }
        warp_to_level(next);
        set_flavor_hint();
        s_flavor_delay = 360u;
        save_progress();
        return;
    }

    if (t == TILE_EXIT && s_player.level == 2u) {
        if ((s_shard_bits & (1u << 2u)) == 0u) {
            set_msg("the exit is sealed; find this level's shard");
        } else if (s_shard_bits == 0x07u) {
            warp_to_level(3);
            set_msg("door opens into impossible calm");
            save_progress();
        } else {
            set_msg("the lock rejects partial shards");
        }
        return;
    }

    set_msg("nothing answers");
}

static void tick_watchers(void) {
    uint8_t i;
    uint8_t stage = sanity_stage();
    uint8_t decay = (s_effect_flags & FX_MIND) ? 2u : 1u;
    uint16_t decay_frames = SANITY_DECAY_FRAMES;

    if (s_anti_flags & AFX_MIND) {
        decay_frames = (uint16_t)(SANITY_DECAY_FRAMES + (SANITY_DECAY_FRAMES / 2u));
        if (decay > 1u) {
            decay--;
        }
    }
    s_sanity_decay_tick++;
    if (s_sanity_decay_tick >= decay_frames) {
        s_sanity_decay_tick = 0;
        if (s_sanity > decay) {
            s_sanity = (uint8_t)(s_sanity - decay);
        } else {
            s_sanity = 0u;
        }
    }
    if (s_contact_cooldown > 0u) {
        s_contact_cooldown--;
    }

    for (i = 0; i < s_entity_count; i++) {
        entity_t *e = &s_entities[i];
        int32_t dx;
        int32_t dy;
        int64_t d2;
        if (!e->alive || e->level != s_player.level) {
            continue;
        }

        if (e->type == ENTITY_WATCHER || e->type == ENTITY_WHISPER) {
            if (stage >= 3u) {
                int32_t nx = e->x;
                int32_t ny = e->y;
                if (s_player.x > e->x) nx += FIX_ONE / 12;
                if (s_player.x < e->x) nx -= FIX_ONE / 12;
                if (s_player.y > e->y) ny += FIX_ONE / 12;
                if (s_player.y < e->y) ny -= FIX_ONE / 12;
                {
                    int16_t tx = (int16_t)(nx >> FIX_SHIFT);
                    int16_t ty = (int16_t)(ny >> FIX_SHIFT);
                    if (!tile_blocks(map_at(e->level, tx, ty))) {
                        e->x = nx;
                        e->y = ny;
                    }
                }
            } else if ((prng8() & 3u) == 0u) {
                int8_t sx = (int8_t)((prng8() & 1u) ? 1 : -1);
                int8_t sy = (int8_t)((prng8() & 1u) ? 1 : -1);
                int32_t nx = e->x + ((int32_t)sx * (FIX_ONE / 10));
                int32_t ny = e->y + ((int32_t)sy * (FIX_ONE / 10));
                int16_t tx = (int16_t)(nx >> FIX_SHIFT);
                int16_t ty = (int16_t)(ny >> FIX_SHIFT);
                if (!tile_blocks(map_at(e->level, tx, ty))) {
                    e->x = nx;
                    e->y = ny;
                }
            }

            dx = e->x - s_player.x;
            dy = e->y - s_player.y;
            d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            if (s_contact_cooldown == 0u && d2 < ((int64_t)FIX_ONE * FIX_ONE) / 3) {
                uint8_t dmg = (uint8_t)((stage >= 3u) ? SANITY_HIT_PANIC : SANITY_HIT_NORMAL);
                if (s_sanity > dmg) {
                    s_sanity = (uint8_t)(s_sanity - dmg);
                } else {
                    s_sanity = 0u;
                }
                s_contact_cooldown = SANITY_CONTACT_COOLDOWN;
                if (stage >= 3u) {
                    set_msg("panic spikes");
                } else {
                    set_msg("something brushes past you");
                }
            }
        }
    }

    if (s_sanity == 0u) {
        s_defeat = true;
    }

    if ((s_effect_flags & FX_TIME) && s_hist_fill > 96u) {
        if (s_time_next > 0u) {
            s_time_next--;
        } else {
            if ((s_anti_flags & AFX_TIME) && ((prng8() & 3u) != 0u)) {
                s_time_next = (uint16_t)(220u + (prng8() & 127u));
            } else {
                uint8_t rewind = (uint8_t)(64u + (prng8() & 31u));
                if (rewind < s_hist_fill) {
                    uint8_t idx = (uint8_t)((s_hist_head + HISTORY_LEN - rewind) % HISTORY_LEN);
                    if (s_history[idx].level == s_player.level) {
                        s_player.x = s_history[idx].x;
                        s_player.y = s_history[idx].y;
                        s_player.angle = s_history[idx].angle;
                        set_msg("time snaps backward");
                    }
                }
                s_time_next = (uint16_t)(360u + (prng8() & 255u));
            }
        }
    }

    if (s_effect_flags & FX_SIGHT) {
        if (s_sight_dark_ticks > 0u) {
            s_sight_dark_ticks--;
        } else if (s_sight_dark_cooldown > 0u) {
            s_sight_dark_cooldown--;
        } else if ((prng8() & 31u) == 0u) {
            s_sight_dark_ticks = (uint8_t)(12u + (prng8() & 15u));
            s_sight_dark_cooldown = (uint16_t)(220u + (prng8() & 127u));
        }
    } else {
        s_sight_dark_ticks = 0u;
        s_sight_dark_cooldown = 180u;
    }
}

static void render_sight_darkness_overlay(void) {
    int m;
    if (s_sight_dark_ticks == 0u) {
        return;
    }
    m = 18 + ((int)s_sight_dark_ticks << 1);
    if (m > 72) {
        m = 72;
    }
    gfx_SetColor(0x00);
    gfx_FillRectangle_NoClip(0, VIEW_TOP, SCREEN_W, m);
    gfx_FillRectangle_NoClip(0, VIEW_BOTTOM - m + 1, SCREEN_W, m);
    gfx_FillRectangle_NoClip(0, VIEW_TOP + m, m, (VIEW_BOTTOM - VIEW_TOP + 1) - (m << 1));
    gfx_FillRectangle_NoClip(SCREEN_W - m, VIEW_TOP + m, m, (VIEW_BOTTOM - VIEW_TOP + 1) - (m << 1));
}

static uint8_t sprite_px_8x32(const uint8_t *spr, uint8_t x, uint8_t y) {
    return spr[2 + ((uint16_t)y << 3) + x];
}

static void draw_textured_column(int x0, int width, int wall_top, int wall_bottom, uint8_t tex_x, uint8_t level, uint8_t hit_tile) {
    int h;
    int y;
    int y_step;
    int32_t tex_pos;
    int32_t tex_step;
    if (wall_top < VIEW_TOP) {
        wall_top = VIEW_TOP;
    }
    if (wall_bottom > VIEW_BOTTOM) {
        wall_bottom = VIEW_BOTTOM;
    }
    if (wall_bottom < wall_top || width <= 0) {
        return;
    }
    h = wall_bottom - wall_top + 1;
    tex_step = ((level == 0u) ? (TEX_H << FIX_SHIFT) : (TEX_SIZE << FIX_SHIFT)) / h;
    tex_pos = 0;
    y_step = s_fast_render ? 3 : 2;
    for (y = wall_top; y <= wall_bottom; y += y_step) {
        int draw_h = y_step;
        uint8_t col;
        if (level == 0u) {
            uint8_t ty = (uint8_t)((tex_pos >> FIX_SHIFT) & (TEX_H - 1));
            if (hit_tile == TILE_WARP || hit_tile == TILE_EXIT) {
                col = sprite_px_8x32(lvl_exit_data, tex_x, ty);
            } else {
                col = sprite_px_8x32(lvl0_wall_data, tex_x, ty);
            }
        } else {
            uint8_t ty = (uint8_t)((tex_pos >> FIX_SHIFT) & (TEX_SIZE - 1));
            col = (s_shard_bits & (1u << level)) ? s_wall_tex[level][ty][tex_x] : s_wall_tex_cached[level][ty][tex_x];
        }
        if (y + draw_h - 1 > wall_bottom) {
            draw_h = wall_bottom - y + 1;
        }
        if (level != 0u && hit_tile == TILE_WARP) {
            col = ((y >> 2) & 1) ? 0x9D : 0xF7;
        } else if (level != 0u && hit_tile == TILE_EXIT) {
            col = ((y >> 2) & 1) ? 0xE6 : 0xF7;
        } else if (hit_tile == TILE_CONSOLE) {
            col = ((y >> 2) & 1) ? 0x24 : 0x34;
        }
        if (col == 0u) {
            tex_pos += tex_step * y_step;
            continue;
        }
        gfx_SetColor(col);
        gfx_FillRectangle_NoClip(x0, y, width, draw_h);
        tex_pos += tex_step * y_step;
    }
}

static void render_planes_fast(uint8_t level) {
    if (level == 0u) {
        int y;
        int mid = SCREEN_H / 2;
        int top_h = mid - VIEW_TOP;
        int bot_h = VIEW_BOTTOM - mid + 1;
        for (y = VIEW_TOP; y < mid; y += 2) {
            uint8_t ty = (uint8_t)((((y - VIEW_TOP) * TEX_H) / top_h) & (TEX_H - 1));
            uint8_t col = sprite_px_8x32(lvl0_ceiling_data, 4, ty);
            if (col != 0u) {
                gfx_SetColor(col);
                gfx_FillRectangle_NoClip(0, y, SCREEN_W, (y + 1 < mid) ? 2 : 1);
            }
        }
        for (y = mid; y <= VIEW_BOTTOM; y += 2) {
            uint8_t ty = (uint8_t)((((y - mid) * TEX_H) / bot_h) & (TEX_H - 1));
            uint8_t col = sprite_px_8x32(lvl0_floor_data, 4, ty);
            if (col != 0u) {
                gfx_SetColor(col);
                gfx_FillRectangle_NoClip(0, y, SCREEN_W, (y + 1 <= VIEW_BOTTOM) ? 2 : 1);
            }
        }
    } else {
        bool clean = (s_shard_bits & (1u << level)) != 0u;
        uint8_t ceil_col = clean ? s_ceil_tex[level][3][3] : s_ceil_tex_cached[level][3][3];
        uint8_t floor_col = clean ? s_floor_tex[level][4][4] : s_floor_tex_cached[level][4][4];
        gfx_SetColor(ceil_col);
        gfx_FillRectangle_NoClip(0, VIEW_TOP, SCREEN_W, (SCREEN_H / 2) - VIEW_TOP);
        gfx_SetColor(floor_col);
        gfx_FillRectangle_NoClip(0, SCREEN_H / 2, SCREEN_W, VIEW_BOTTOM - (SCREEN_H / 2) + 1);
    }
}

static void render_world(void) {
    int32_t posX = s_player.x;
    int32_t posY = s_player.y;
    int32_t dirX = s_cos[s_player.angle];
    int32_t dirY = s_sin[s_player.angle];
    const uint8_t (*lvl_map)[MAP_W] = s_map[s_player.level];
    int32_t planeX = (-dirY * 2720) >> TRIG_SHIFT;
    int32_t planeY = (dirX * 2720) >> TRIG_SHIFT;
    const uint16_t *edges;
    const int16_t *cam_tbl;
    uint8_t ray_cols;
    uint8_t c;

    s_fast_render = (bool)(s_run_held || s_turning);
    ray_cols = s_fast_render ? RAY_COLS_LOW : RAY_COLS_HIGH;
    s_active_cols = ray_cols;
    edges = s_fast_render ? s_col_edges_low : s_col_edges_high;
    cam_tbl = s_fast_render ? s_camx_low : s_camx_high;
    render_planes_fast(s_player.level);

    for (c = 0; c < ray_cols; c++) {
        int32_t hitPos;
        int32_t camX = cam_tbl[c];
        int32_t rayDirX = dirX + ((planeX * camX) >> TRIG_SHIFT);
        int32_t rayDirY = dirY + ((planeY * camX) >> TRIG_SHIFT);
        uint32_t absRayX = (uint32_t)(rayDirX < 0 ? -rayDirX : rayDirX);
        uint32_t absRayY = (uint32_t)(rayDirY < 0 ? -rayDirY : rayDirY);
        uint16_t idxX;
        uint16_t idxY;
        int16_t mapX = (int16_t)(posX >> FIX_SHIFT);
        int16_t mapY = (int16_t)(posY >> FIX_SHIFT);
        uint32_t recipX;
        uint32_t recipY;
        uint32_t deltaDistX;
        uint32_t deltaDistY;
        uint32_t sideDistX;
        uint32_t sideDistY;
        int8_t stepX;
        int8_t stepY;
        uint8_t hit = 0;
        uint8_t dda_steps = 0;
        uint8_t side = 0;
        uint8_t hit_tile = TILE_WALL;
        uint8_t tex_x;
        uint32_t perpDist;
        int32_t lineH;
        int32_t drawStart;
        int32_t drawEnd;
        int x0 = edges[c];
        int x1 = edges[c + 1u];
        int col_w = x1 - x0;

        idxX = (uint16_t)((absRayX >= RECIP_LUT_SIZE) ? (RECIP_LUT_SIZE - 1) : absRayX);
        idxY = (uint16_t)((absRayY >= RECIP_LUT_SIZE) ? (RECIP_LUT_SIZE - 1) : absRayY);
        recipX = s_recip_lut[idxX];
        recipY = s_recip_lut[idxY];
        deltaDistX = (rayDirX == 0) ? 0x3FFFFFFFu : recipX;
        deltaDistY = (rayDirY == 0) ? 0x3FFFFFFFu : recipY;

        if (rayDirX < 0) {
            int32_t numX = (posX - ((int32_t)mapX << FIX_SHIFT)) << TRIG_SHIFT;
            stepX = -1;
            sideDistX = (uint32_t)(((int64_t)numX * recipX) >> 28);
        } else {
            int32_t numX = ((((int32_t)mapX + 1) << FIX_SHIFT) - posX) << TRIG_SHIFT;
            stepX = 1;
            sideDistX = (rayDirX == 0) ? 0x3FFFFFFFu : (uint32_t)(((int64_t)numX * recipX) >> 28);
        }

        if (rayDirY < 0) {
            int32_t numY = (posY - ((int32_t)mapY << FIX_SHIFT)) << TRIG_SHIFT;
            stepY = -1;
            sideDistY = (uint32_t)(((int64_t)numY * recipY) >> 28);
        } else {
            int32_t numY = ((((int32_t)mapY + 1) << FIX_SHIFT) - posY) << TRIG_SHIFT;
            stepY = 1;
            sideDistY = (rayDirY == 0) ? 0x3FFFFFFFu : (uint32_t)(((int64_t)numY * recipY) >> 28);
        }

        while (!hit && dda_steps < DDA_STEP_CAP) {
            if (sideDistX < sideDistY) {
                sideDistX += deltaDistX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaDistY;
                mapY += stepY;
                side = 1;
            }
            if ((uint16_t)mapX < MAP_W && (uint16_t)mapY < MAP_H) {
                hit_tile = lvl_map[mapY][mapX];
            } else {
                hit_tile = TILE_WALL;
            }
            if (hit_tile != TILE_EMPTY) {
                hit = 1;
            }
            dda_steps++;
        }
        if (!hit) {
            hit = 1;
            hit_tile = TILE_WALL;
        }

        perpDist = (side == 0) ? (sideDistX - deltaDistX) : (sideDistY - deltaDistY);
        if (perpDist < 256) {
            perpDist = 256;
        }

        s_zbuf[c] = (uint16_t)((perpDist > 0xFFFFu) ? 0xFFFFu : perpDist);

        /* perpDist is Q16, so scale once by FIX_ONE and do not shift again. */
        lineH = ((int32_t)SCREEN_H * FIX_ONE) / (int32_t)perpDist;
        if (lineH < 1) lineH = 1;
        if (lineH > SCREEN_H) lineH = SCREEN_H;

        drawStart = (SCREEN_H / 2) - (lineH / 2);
        drawEnd = drawStart + lineH;
        if (side == 0u) {
            hitPos = posY + (int32_t)((((int64_t)rayDirY * perpDist) >> TRIG_SHIFT));
        } else {
            hitPos = posX + (int32_t)((((int64_t)rayDirX * perpDist) >> TRIG_SHIFT));
        }
        tex_x = (uint8_t)((hitPos >> (FIX_SHIFT - 3)) & (TEX_SIZE - 1));
        if ((side == 0u && rayDirX > 0) || (side == 1u && rayDirY < 0)) {
            tex_x = (uint8_t)((TEX_SIZE - 1) - tex_x);
        }
        if (col_w < 1) {
            col_w = 1;
        }
        draw_textured_column(x0, col_w, (int)drawStart, (int)drawEnd, tex_x, s_player.level, hit_tile);
    }
}

static void render_entities(void) {
    uint8_t i;
    int sprite_step = s_fast_render ? 3 : 2;
    const uint8_t *x_to_ray = (s_active_cols == RAY_COLS_HIGH) ? s_x_to_ray_high : s_x_to_ray_low;
    uint8_t stage = sanity_stage();
    int32_t dirX = s_cos[s_player.angle];
    int32_t dirY = s_sin[s_player.angle];

    for (i = 0; i < s_entity_count; i++) {
        const entity_t *e = &s_entities[i];
        int32_t dx;
        int32_t dy;
        int32_t fwd;
        int32_t lat;
        int32_t sx;
        int32_t h;
        int32_t w;
        int32_t y0;
        int32_t y1;
        int32_t x0;
        int32_t x1;
        int32_t x;
        uint8_t col;
        const uint8_t *sprite_data = NULL;

        if (!e->alive || e->level != s_player.level) {
            continue;
        }

        dx = e->x - s_player.x;
        dy = e->y - s_player.y;

        fwd = (int32_t)((((int64_t)dx * dirX) + ((int64_t)dy * dirY)) >> FIX_SHIFT); /* Q12 */
        lat = (int32_t)((((int64_t)dx * -dirY) + ((int64_t)dy * dirX)) >> FIX_SHIFT); /* Q12 */

        if (fwd <= (TRIG_ONE / 8)) {
            continue;
        }

        sx = (SCREEN_W / 2) + (int32_t)(((int64_t)lat * (SCREEN_W / 2)) / fwd);
        h = (int32_t)(((int64_t)SPRITE_REF_H * TRIG_ONE) / fwd);
        w = h / 2;
        if (w < 2) w = 2;
        if (h < 2) h = 2;

        y0 = (SCREEN_H / 2) - (h / 2);
        x0 = sx - (w / 2);
        x1 = sx + (w / 2);
        if (y0 < 0) {
            y0 = 0;
        }
        if (y0 + h >= SCREEN_H) {
            h = SCREEN_H - y0 - 1;
        }
        if (h < 2) {
            h = 2;
        }
        y1 = y0 + h;

        if (e->type == ENTITY_FRAGMENT) {
            if (s_player.level == 0u) {
                sprite_data = shard_data;
                col = 0xF7;
            } else {
                col = 0xF7;
                h += (int32_t)((s_frame >> 2) & 3u);
            }
        } else if (e->type == ENTITY_ANTI) {
            col = 0x9D;
        } else if (e->type == ENTITY_TONIC) {
            if (s_player.level == 0u) {
                sprite_data = sanity_juice_data;
                col = 0x9F;
            } else {
                col = 0x9F;
            }
        } else if (e->type == ENTITY_WATCHER) {
            if (s_player.level == 0u) {
                sprite_data = lvl0_entity_data;
                col = 0xE5;
            } else {
                col = (stage >= 3u) ? 0x1F : ((s_player.level == 1) ? 0x24 : 0x14);
            }
        } else {
            if (s_player.level == 0u) {
                sprite_data = lvl0_entity_data;
                col = 0xED;
            } else {
                col = (stage >= 3u) ? 0x2F : 0xB8;
            }
        }

        for (x = x0; x <= x1; x += sprite_step) {
            int32_t ray_idx;
            if (x < 0 || x >= SCREEN_W - 1) {
                continue;
            }
            ray_idx = x_to_ray[x];
            if (ray_idx < 0 || ray_idx >= s_active_cols) {
                continue;
            }

            if (((uint32_t)(fwd << 4) >= s_zbuf[ray_idx])) {
                continue;
            }

            if (sprite_data != NULL) {
                uint8_t tex_x = (uint8_t)((((x - x0) * TEX_SIZE) / (w > 0 ? w : 1)) & (TEX_SIZE - 1));
                int y;
                int y_step = s_fast_render ? 3 : 2;
                for (y = (int)y0; y <= (int)y1; y += y_step) {
                    int draw_h = y_step;
                    uint8_t tex_y = (uint8_t)((((y - (int)y0) * TEX_H) / (h > 0 ? h : 1)) & (TEX_H - 1));
                    uint8_t p = sprite_px_8x32(sprite_data, tex_x, tex_y);
                    if (p == 0u) {
                        continue;
                    }
                    if (y + draw_h > (int)y1) {
                        draw_h = (int)y1 - y + 1;
                    }
                    gfx_SetColor(p);
                    gfx_FillRectangle_NoClip((int)x, y, sprite_step, draw_h);
                }
            } else if (e->type == ENTITY_FRAGMENT) {
                uint8_t g = (uint8_t)((((x >> 1) + (y0 >> 1) + s_frame) & 7u));
                uint8_t gcol = s_shard_glitch[g];
                int ydraw = (int)y0 + (int)((s_frame + x) & 1u);
                if (((x + s_frame) & 3u) == 0u) {
                    continue;
                }
                gfx_SetColor(gcol);
                gfx_FillRectangle_NoClip((int)x, ydraw, sprite_step, (y0 + h < SCREEN_H ? y0 + h : SCREEN_H - 1) - y0);
            } else if (e->type == ENTITY_ANTI) {
                gfx_SetColor((uint8_t)(col - (uint8_t)(((x >> 1) + s_frame) & 1u)));
                gfx_FillRectangle_NoClip((int)x, (int)y0, sprite_step, (y0 + h < SCREEN_H ? y0 + h : SCREEN_H - 1) - y0);
            } else if (e->type == ENTITY_TONIC) {
                gfx_SetColor((uint8_t)(col - (uint8_t)((s_frame + x) & 1u)));
                gfx_FillRectangle_NoClip((int)x, (int)y0, sprite_step, (y0 + h < SCREEN_H ? y0 + h : SCREEN_H - 1) - y0);
            } else {
                gfx_SetColor((uint8_t)(col - (uint8_t)((x + y0) & 3u)));
                gfx_FillRectangle_NoClip((int)x, (int)y0, sprite_step, (y0 + h < SCREEN_H ? y0 + h : SCREEN_H - 1) - y0);
            }
        }
    }

    if (s_effect_flags & FX_SIGHT) {
        /* Subtle hallucinations: sparse ghost motes instead of full moving bars. */
        uint8_t k;
        for (k = 0; k < 4u; k++) {
            if (((s_frame + (k * 11u)) & 7u) != 0u) {
                continue;
            }
            {
                int x = (int)(((uint16_t)(s_frame * (5u + k) + (k * 73u))) % SCREEN_W);
                int y = 24 + (int)(((uint16_t)(s_frame * (3u + k) + (k * 37u))) % (SCREEN_H - 48));
                uint8_t c = (k & 1u) ? 0xE6 : 0xF7;
                if ((s_anti_flags & AFX_MIND) && (k & 1u)) {
                    continue;
                }
                gfx_SetColor(c);
                gfx_FillRectangle_NoClip(x, y, 2, 2);
            }
        }
    }
}

static void draw_hud(void) {
    char buf[30];
    char hint[26];
    gfx_SetColor(0x00);
    gfx_FillRectangle_NoClip(0, 0, SCREEN_W, 14);
    gfx_SetColor(0xE5);
    gfx_HorizLine_NoClip(0, 13, SCREEN_W);
    gfx_SetTextFGColor(0xF7);
    gfx_SetTextBGColor(0x00);
    sprintf(buf, "L%u  S:%u%%  shards:%u  phase:%u", (unsigned int)(s_player.level + 1u), (unsigned int)s_sanity, (unsigned int)s_fragments, (unsigned int)(s_console_bits & 7u));
    gfx_PrintStringXY(buf, 3, 3);
    if (s_player.level < 3u && (s_shard_bits & (1u << s_player.level))) {
        int16_t px = (int16_t)(s_player.x >> FIX_SHIFT);
        int16_t py = (int16_t)(s_player.y >> FIX_SHIFT);
        int16_t tx = (s_player.level < 2u) ? s_warp_x[s_player.level] : s_warp_x[2];
        int16_t ty = (s_player.level < 2u) ? s_warp_y[s_player.level] : s_warp_y[2];
        int16_t dx = tx - px;
        int16_t dy = ty - py;
        if ((dx >= 0 ? dx : -dx) > (dy >= 0 ? dy : -dy)) {
            sprintf(hint, "obj:%s", (dx > 0) ? "E" : "W");
        } else {
            sprintf(hint, "obj:%s", (dy > 0) ? "S" : "N");
        }
        gfx_PrintStringXY(hint, 180, 3);
    }
    if ((s_anti_flags & AFX_SIGHT) && s_player.level < 3u) {
        int16_t px = (int16_t)(s_player.x >> FIX_SHIFT);
        int16_t py = (int16_t)(s_player.y >> FIX_SHIFT);
        int16_t tx = 0;
        int16_t ty = 0;
        int16_t dx;
        int16_t dy;
        if ((s_shard_bits & (1u << s_player.level)) == 0u) {
            uint8_t i;
            for (i = 0; i < s_entity_count; i++) {
                if (s_entities[i].alive && s_entities[i].level == s_player.level && s_entities[i].type == ENTITY_FRAGMENT) {
                    tx = (int16_t)(s_entities[i].x >> FIX_SHIFT);
                    ty = (int16_t)(s_entities[i].y >> FIX_SHIFT);
                    break;
                }
            }
        } else if (s_player.level < 2u) {
            tx = s_warp_x[s_player.level];
            ty = s_warp_y[s_player.level];
        } else {
            tx = s_warp_x[2];
            ty = s_warp_y[2];
        }
        dx = tx - px;
        dy = ty - py;
        if ((dx > 1 || dx < -1) || (dy > 1 || dy < -1)) {
            if ((dx >= 0 ? dx : -dx) > (dy >= 0 ? dy : -dy)) {
                sprintf(hint, "guide:%s", (dx > 0) ? "E" : "W");
            } else {
                sprintf(hint, "guide:%s", (dy > 0) ? "S" : "N");
            }
        } else {
            sprintf(hint, "guide:close");
        }
        gfx_PrintStringXY(hint, 226, 3);
    }

    if (s_msg_timer) {
        gfx_SetColor(0x00);
        gfx_FillRectangle_NoClip(0, SCREEN_H - 14, SCREEN_W, 14);
        gfx_SetColor(0xE5);
        gfx_HorizLine_NoClip(0, SCREEN_H - 14, SCREEN_W);
        gfx_SetTextFGColor(0xF7);
        gfx_PrintStringXY(s_msg, 3, SCREEN_H - 10);
        s_msg_timer--;
    }

    if (s_player.level == 3u) {
        gfx_SetColor(0);
        gfx_FillRectangle_NoClip(40, 100, 240, 40);
        gfx_SetTextFGColor(255);
        gfx_PrintStringXY("you made it out", 110, 112);
        gfx_PrintStringXY("or deeper in", 112, 124);
    }

    if (s_defeat) {
        gfx_SetColor(0);
        gfx_FillRectangle_NoClip(52, 96, 216, 48);
        gfx_SetTextFGColor(0xF7);
        gfx_PrintStringXY("sanity collapsed", 108, 110);
        gfx_PrintStringXY("press clear", 122, 124);
    }
}

static void update_input(void) {
    int32_t dirX = s_cos[s_player.angle];
    int32_t dirY = s_sin[s_player.angle];
    int32_t move_speed = MOVE_SPEED;
    int32_t strafe_speed = STRAFE_SPEED;
    int32_t move_x;
    int32_t move_y;
    int32_t strafe_x;
    int32_t strafe_y;

    kb_Scan();
    s_run_held = kb_IsDown(kb_KeyAlpha);
    s_turning = (bool)(kb_IsDown(kb_KeyLeft) || kb_IsDown(kb_KeyRight));

    if (s_effect_flags & FX_MOVEMENT) {
        if (s_invert_active > 0u) {
            s_invert_active--;
        } else if (s_invert_next > 0u) {
            s_invert_next--;
        } else {
            if ((s_anti_flags & AFX_MOVEMENT) && ((prng8() & 3u) != 0u)) {
                s_invert_next = (uint16_t)(130u + (prng8() & 63u));
            } else {
                s_invert_active = (uint8_t)(45u + (prng8() & 31u));
                s_invert_next = (uint16_t)(240u + (prng8() & 127u));
                if (s_msg_timer < 20u) {
                    set_msg("controls desync");
                }
            }
        }
    }

    if (s_run_held) {
        move_speed = (MOVE_SPEED * RUN_MULT_NUM) / RUN_MULT_DEN;
        strafe_speed = (STRAFE_SPEED * RUN_MULT_NUM) / RUN_MULT_DEN;
    }
    move_x = (dirX * move_speed) >> TRIG_SHIFT;
    move_y = (dirY * move_speed) >> TRIG_SHIFT;
    strafe_x = (-dirY * strafe_speed) >> TRIG_SHIFT;
    strafe_y = (dirX * strafe_speed) >> TRIG_SHIFT;

    if (s_invert_active > 0u) {
        if (kb_IsDown(kb_KeyLeft)) {
            s_player.angle = (uint8_t)(s_player.angle + TURN_SPEED);
            s_turned_this_tick = true;
        }
        if (kb_IsDown(kb_KeyRight)) {
            s_player.angle = (uint8_t)(s_player.angle - TURN_SPEED);
            s_turned_this_tick = true;
        }
        if (kb_IsDown(kb_KeyUp)) {
            move_player(-move_x, -move_y);
        }
        if (kb_IsDown(kb_KeyDown)) {
            move_player(move_x, move_y);
        }
        if (kb_IsDown(kb_KeyYequ)) {
            move_player(-strafe_x, -strafe_y);
        }
        if (kb_IsDown(kb_KeyWindow)) {
            move_player(strafe_x, strafe_y);
        }
    } else {
        if (kb_IsDown(kb_KeyLeft)) {
            s_player.angle = (uint8_t)(s_player.angle - TURN_SPEED);
            s_turned_this_tick = true;
        }
        if (kb_IsDown(kb_KeyRight)) {
            s_player.angle = (uint8_t)(s_player.angle + TURN_SPEED);
            s_turned_this_tick = true;
        }
        if (kb_IsDown(kb_KeyUp)) {
            move_player(move_x, move_y);
        }
        if (kb_IsDown(kb_KeyDown)) {
            move_player(-move_x, -move_y);
        }
        if (kb_IsDown(kb_KeyYequ)) {
            move_player(strafe_x, strafe_y);
        }
        if (kb_IsDown(kb_KeyWindow)) {
            move_player(-strafe_x, -strafe_y);
        }
    }

    if (kb_IsDown(kb_Key2nd)) {
        queue_push(ACT_INTERACT);
        while (kb_IsDown(kb_Key2nd)) {
            kb_Scan();
        }
    }
}

int main(void) {
    init_trig();
    init_render_tables();
    s_player.x = (2 << FIX_SHIFT) + (FIX_ONE / 2);
    s_player.y = (2 << FIX_SHIFT) + (FIX_ONE / 2);
    s_player.angle = 0;
    s_player.level = 0;

    s_console_bits = 0;
    s_sanity = SANITY_MAX;
    s_sanity_decay_tick = 0;
    s_contact_cooldown = 0;
    s_defeat = false;
    s_shard_bits = 0;
    s_effect_flags = 0;
    s_anti_bits = 0;
    s_anti_flags = 0;
    s_invert_next = 0;
    s_invert_active = 0;
    s_time_next = 0;
    s_sight_dark_ticks = 0;
    s_sight_dark_cooldown = 180;
    s_hist_head = 0;
    s_hist_fill = 0;
    s_flavor_delay = 90;
    s_action_head = 0;
    s_action_tail = 0;
    s_action_count = 0;
    s_msg[0] = '\0';
    s_msg_timer = 0;

    load_progress();
    sync_effects_from_shards();
    sync_anti_effects_from_bits();
    init_maps();
    apply_shard_progress();

    spawn_entities();
    apply_entity_progress();
    set_flavor_hint();
    rebuild_corruption_cache(sanity_stage());

    gfx_Begin();
    gfx_SetPalette(mypalette, sizeof_mypalette, 0);
    gfx_SetDrawBuffer();

    while (!kb_IsDown(kb_KeyClear)) {
        uint8_t budget = ACTION_BUDGET;
        action_t act;

        if (s_action_count == 0u) {
            queue_push(ACT_TICK);
        }

        while (budget-- > 0u && queue_pop(&act)) {
            switch (act) {
                case ACT_TICK: {
                    uint8_t cur_stage;
                    bool need_redraw;
                    s_frame++;
                    s_moved_this_tick = false;
                    s_turned_this_tick = false;
                    if (!s_defeat) {
                        tick_time_history();
                        update_input();
                        tick_watchers();
                        check_pickups();
                    }
                    cur_stage = sanity_stage();
                    if (cur_stage != s_cached_stage) {
                        rebuild_corruption_cache(cur_stage);
                    }
                    tick_flavor_hint();

                    need_redraw = s_moved_this_tick || s_turned_this_tick || s_turning || s_run_held ||
                                  (s_msg_timer > 0u) || s_defeat || (s_sight_dark_ticks > 0u) ||
                                  ((s_effect_flags & FX_SIGHT) != 0u) || ((s_frame & 3u) == 0u);
                    if (need_redraw) {
                        queue_push(ACT_RENDER_WORLD);
                        queue_push(ACT_RENDER_ENTITIES);
                        queue_push(ACT_RENDER_OVERLAY);
                        queue_push(ACT_RENDER_HUD);
                        queue_push(ACT_SWAP);
                    }
                } break;
                case ACT_INTERACT:
                    interact();
                    break;
                case ACT_RENDER_WORLD:
                    render_world();
                    break;
                case ACT_RENDER_ENTITIES:
                    render_entities();
                    break;
                case ACT_RENDER_OVERLAY:
                    render_sight_darkness_overlay();
                    break;
                case ACT_RENDER_HUD:
                    draw_hud();
                    break;
                case ACT_SWAP:
                    gfx_SwapDraw();
                    break;
                default:
                    break;
            }
        }
    }

    save_progress();
    gfx_End();
    return 0;
}
