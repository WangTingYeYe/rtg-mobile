/* Adaptive left-half hints: the scene table and path parser are the portable
 * part of Touchable's rhythm-heaven adapter (no guest addresses). */
#include "../touch_hint.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* hle_fs.c owns this in the real build; the test only needs the symbol. */
void (*g_dm_fs_open_hook)(const char *path);

static int layout(void) { return DM_TOUCH_LAYOUT(dm_android_touch_hint_get()); }
static int first(void) { return DM_TOUCH_FIRST(dm_android_touch_hint_get()); }
static int second(void) { return DM_TOUCH_SECOND(dm_android_touch_hint_get()); }
static int extra(void) { return DM_TOUCH_EXTRA(dm_android_touch_hint_get()); }

int main(void) {
    assert(layout() == DM_TOUCH_MENU);
    /* Paths outside graph/ and shared scene folders do not change anything. */
    assert(!dm_android_touch_hint_path("data:/mesg/CN/CNzh/stage_parasol_00.msbt"));
    assert(!dm_android_touch_hint_path("data:/graph/scene/common/info.spr"));
    dm_android_touch_hint_path("data:/graph/menu/main_menu/main.spr");
    assert(layout() == DM_TOUCH_MENU);

    /* A-only minigame: the whole left half presses A. */
    dm_android_touch_hint_path("data:/graph/prologue/flydisc/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 0);
    dm_android_touch_hint_path("data:/graph/scene/common/pause.spr");
    dm_android_touch_hint_path("data:/graph/scene/flydisc/type_00/main.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 0);
    assert(!dm_android_touch_hint_path("data:/graph/scene/flydisc/type_00/bg.spr"));
    dm_android_touch_hint_path("data:/graph/menu/main_menu/main.spr");
    assert(layout() == DM_TOUCH_MENU);

    /* One direction, entered without a title card. */
    dm_android_touch_hint_path("data:/graph/scene/parasol/type_00/main.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 15);
    dm_android_touch_hint_path("data:/graph/result/result.spr");
    assert(layout() == DM_TOUCH_MENU);

    /* Left and right steps use the compact D-pad. */
    dm_android_touch_hint_path("data:/graph/prologue/dancer/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == (DM_TOUCH_DIR_LEFT | DM_TOUCH_DIR_RIGHT));

    /* B minigame. */
    dm_android_touch_hint_path("data:/graph/prologue/foot_race/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 1);

    /* A remix combines its scenes; a new title card starts over. */
    dm_android_touch_hint_path("data:/graph/prologue/remix/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == 0);
    dm_android_touch_hint_path("data:/graph/scene/ring/type_00/main.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 0);
    dm_android_touch_hint_path("data:/graph/scene/hurdle/type_00/main.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 15);
    dm_android_touch_hint_path("data:/graph/scene/wiper/type_00/main.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == (DM_TOUCH_DIR_LEFT | DM_TOUCH_DIR_DOWN));
    dm_android_touch_hint_path("data:/graph/scene/rolling/type_00/main.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == (DM_TOUCH_DIR_LEFT | DM_TOUCH_DIR_RIGHT | DM_TOUCH_DIR_DOWN));
    dm_android_touch_hint_path("data:/graph/prologue/wiper/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 12);

    /* Many-button and unknown scenes keep the D-pad. */
    dm_android_touch_hint_path("data:/graph/prologue/drummer/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == 0);
    dm_android_touch_hint_path("data:/graph/prologue/new_game/type_00/prologue.spr");
    assert(layout() == DM_TOUCH_DPAD && first() == 0);
    dm_android_touch_hint_path("data:/graph/menu/main_menu/main.spr");
    assert(layout() == DM_TOUCH_MENU);

    /* BeatSpell steps with B on the left half and keeps the D-pad, X and Y. */
    dm_android_touch_hint_path("data:/graph/beatspell/common/common.spr");
    assert(layout() == DM_TOUCH_ONE && first() == 1 && second() == 0);
    assert(extra() == (DM_TOUCH_SHOW_DPAD | DM_TOUCH_SHOW_X | DM_TOUCH_SHOW_Y));
    assert(!dm_android_touch_hint_path("data:/graph/beatspell/bg/castle_00.spr"));
    dm_android_touch_hint_path("data:/graph/menu/main_menu/main.spr");
    assert(layout() == DM_TOUCH_MENU);

    /* The change counter advances only when the hint changes. */
    uint64_t before = dm_android_touch_hint_get();
    dm_android_touch_hint_path("data:/graph/menu/other/main.spr");
    assert(dm_android_touch_hint_get() == before);
    dm_android_touch_hint_path("data:/graph/scene/flydisc/type_00/main.spr");
    assert(dm_android_touch_hint_get() >> 32 > before >> 32);
    puts("t_touch_hint: ok");
    return 0;
}