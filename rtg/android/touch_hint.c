/* Rhythm Heaven touch hints.
 *
 * A minigame shows its title card from graph/prologue/<name>/ and plays from graph/scene/<name>/.
 * Menus read graph/menu/ and results graph/result/; there the phone shows a small D-pad.
 * A remix reads several scene folders after its title card, so the keys of every scene read
 * since the last title card are combined.
 *
 * The table lists the buttons each minigame uses besides A. Y (hold for a demo), - (skip
 * practice) and + (pause) are not listed: the phone has separate buttons for them.
 *
 * Ported from Touchable (MIT), internal/games/rhythm-heaven/runtime/rh_touch.c. The paths are
 * the same for this game build; the state is only driven by guest path strings, not by fixed
 * addresses, so a different scene list still degrades to the compact D-pad. */
#include "touch_hint.h"
#include "../host/dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum { K_A = 1, K_B = 2, K_LEFT = 4, K_UP = 8, K_RIGHT = 16, K_DOWN = 32, K_MANY = 64, K_SPELL = 128 };
typedef struct { const char *name; unsigned keys; } scene_keys;
static const scene_keys scenes[] = {
    {"birds", K_A}, {"block", K_A}, {"cooking", K_A}, {"flydisc", K_A}, {"fugu", K_A}, {"grasp", K_A},
    {"guardian", K_A}, {"hammer", K_A}, {"hungry", K_A}, {"insect", K_A}, {"kaeru", K_A}, {"kukan", K_A},
    {"moon", K_A}, {"pumpup", K_A}, {"putilabo", K_A}, {"rabbit", K_A}, {"ring", K_A}, {"rock", K_A},
    {"rocket", K_A}, {"rope", K_A}, {"samurai", K_A}, {"shinkai", K_A}, {"shooting", K_A}, {"snack", K_A},
    {"spinning", K_A}, {"teatime", K_A}, {"tennis", K_A}, {"the_a", K_A}, {"thunder", K_A}, {"virus", K_A},
    {"volley", K_A},
    {"bacteria", K_A | K_DOWN}, {"blanco", K_A | K_DOWN}, {"cars", K_A | K_DOWN}, {"hurdle", K_A | K_DOWN},
    {"owl", K_A | K_DOWN}, {"parasol", K_A | K_DOWN}, {"skater", K_A | K_DOWN}, {"softcatch", K_A | K_DOWN},
    {"brush", K_A | K_LEFT}, {"clap", K_A | K_LEFT}, {"commander", K_A | K_LEFT}, {"crab", K_A | K_LEFT},
    {"onion", K_A | K_LEFT}, {"soccer", K_A | K_LEFT}, {"talk", K_A | K_LEFT}, {"tono2", K_A | K_LEFT},
    {"trolley", K_A | K_LEFT}, {"wiper", K_A | K_LEFT},
    {"rolling", K_A | K_RIGHT},
    {"dancer", K_A | K_LEFT | K_RIGHT}, {"master", K_A | K_LEFT | K_RIGHT},
    {"foot_race", K_A | K_B}, {"quiz", K_A | K_B},
    /* More buttons than two touch areas hold, or no prompts to read: the left half stays a D-pad. */
    {"audio_tile", K_MANY}, {"drummer", K_MANY}, {"konkon", K_MANY}, {"nervous", K_MANY},
    {"doc_comic", K_MANY}, {"doc_text", K_MANY},
    {"remix", 0},   /* adds no keys itself; its scenes do */
};

typedef struct { int active; unsigned keys; } hint_state;
static hint_state hint;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t packed = DM_TOUCH_MENU;

static unsigned keys_of(const char *name, size_t length) {
    for (size_t i = 0; i < sizeof scenes / sizeof *scenes; ++i)
        if (strlen(scenes[i].name) == length && !memcmp(scenes[i].name, name, length)) return scenes[i].keys;
    return K_MANY;
}

static uint32_t hint_of(const hint_state *state) {
    if (!state->active) return DM_TOUCH_PACK(DM_TOUCH_MENU, 0, 0, 0);
    if (state->keys & K_SPELL)
        return DM_TOUCH_PACK(DM_TOUCH_ONE, 1, 0, DM_TOUCH_SHOW_DPAD | DM_TOUCH_SHOW_X | DM_TOUCH_SHOW_Y);
    if (state->keys & K_MANY) return DM_TOUCH_PACK(DM_TOUCH_DPAD, 0, 0, 0);
    unsigned directions = state->keys & (K_LEFT | K_UP | K_RIGHT | K_DOWN);
    int first = (directions & K_LEFT ? DM_TOUCH_DIR_LEFT : 0) |
                (directions & K_UP ? DM_TOUCH_DIR_UP : 0) |
                (directions & K_RIGHT ? DM_TOUCH_DIR_RIGHT : 0) |
                (directions & K_DOWN ? DM_TOUCH_DIR_DOWN : 0);
    if (directions && (directions & (directions - 1))) return DM_TOUCH_PACK(DM_TOUCH_DPAD, first, 0, 0);
    /* Order of the left-half areas from left to right. */
    static const struct { unsigned key; int button; } order[] = {
        {K_LEFT, 12}, {K_B, 1}, {K_DOWN, 15}, {K_UP, 13}, {K_RIGHT, 14}};
    int found[5], count = 0;
    for (int i = 0; i < 5; ++i) if (state->keys & order[i].key) found[count++] = order[i].button;
    if (count == 0) return DM_TOUCH_PACK(state->keys & K_A ? DM_TOUCH_ONE : DM_TOUCH_DPAD, 0, 0, 0);
    if (count == 1) return DM_TOUCH_PACK(DM_TOUCH_ONE, found[0], 0, 0);
    if (count == 2) return DM_TOUCH_PACK(DM_TOUCH_TWO, found[0], found[1], 0);
    return DM_TOUCH_PACK(DM_TOUCH_DPAD, first, 0, 0);
}

static void publish(uint32_t value) {
    uint64_t old = atomic_load(&packed);
    atomic_store(&packed, (uint64_t)value | ((old >> 32) + 1) << 32);
}

int dm_android_touch_hint_path(const char *path) {
    if (!path) return 0;
    const char *graph = strstr(path, ":/graph/");
    if (!graph) return 0;
    const char *top = graph + 8, *slash = strchr(top, '/');
    if (!slash) return 0;
    size_t top_length = (size_t)(slash - top);
    const char *name = slash + 1, *end = strchr(name, '/');
    size_t name_length = end ? (size_t)(end - name) : 0;
    int changed = 0;
    pthread_mutex_lock(&lock);
    uint32_t before = hint_of(&hint);
#define TOP(text) (top_length == sizeof text - 1 && !memcmp(top, text, top_length))
#define NAME(text) (name_length == sizeof text - 1 && !memcmp(name, text, name_length))
    if (TOP("menu") || TOP("result") || TOP("epilogue") || TOP("boot")) {
        hint.active = 0; hint.keys = 0;
    } else if (TOP("beatspell")) {
        hint.active = 1; hint.keys = K_SPELL;
    } else if (TOP("prologue") && name_length && !NAME("common")) {
        hint.active = 1; hint.keys = keys_of(name, name_length);
    } else if (TOP("scene") && name_length && !NAME("common") && !NAME("multi_common")) {
        if (!hint.active) { hint.active = 1; hint.keys = 0; }
        hint.keys |= keys_of(name, name_length);
    } else {
        pthread_mutex_unlock(&lock);
        return 0;
    }
#undef TOP
#undef NAME
    uint32_t after = hint_of(&hint);
    if (after != before) {
        publish(after);
        LOG("[touch-hint] layout=%d first=%d second=%d extra=%d path=%s",
            DM_TOUCH_LAYOUT(after), DM_TOUCH_FIRST(after), DM_TOUCH_SECOND(after), DM_TOUCH_EXTRA(after), path);
        changed = 1;
    }
    pthread_mutex_unlock(&lock);
    return changed;
}

extern void (*g_dm_fs_open_hook)(const char *path);
static void open_hook(const char *path) { dm_android_touch_hint_path(path); }
void dm_android_touch_hint_install(void) { g_dm_fs_open_hook = open_hook; LOG("[touch-hint] installed"); }
uint64_t dm_android_touch_hint_get(void) { return atomic_load(&packed); }