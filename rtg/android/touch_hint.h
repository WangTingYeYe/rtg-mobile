#pragma once
#include <stdint.h>
/* Touch hints: which buttons the left half of the phone screen presses.
 * Ported from Touchable (MIT), internal/games/rhythm-heaven/runtime/rh_touch.c.
 * Layout values match Touchable's ss_touch.h; button numbers are nn::hid::NpadButton bits. */
enum { DM_TOUCH_DPAD = 0, DM_TOUCH_ONE = 1, DM_TOUCH_TWO = 2, DM_TOUCH_MENU = 3 };
enum { DM_TOUCH_DIR_LEFT = 1, DM_TOUCH_DIR_UP = 2, DM_TOUCH_DIR_RIGHT = 4, DM_TOUCH_DIR_DOWN = 8 };
enum { DM_TOUCH_SHOW_DPAD = 1, DM_TOUCH_SHOW_X = 2, DM_TOUCH_SHOW_Y = 4 };
/* Packed: layout | first << 8 | second << 16 | extra << 24 | change count << 32. Starts as DM_TOUCH_MENU. */
#define DM_TOUCH_PACK(layout,first,second,extra) \
    (((uint32_t)(layout)&0xffu)|(((uint32_t)(first)&0xffu)<<8)|(((uint32_t)(second)&0xffu)<<16)|(((uint32_t)(extra)&0xffu)<<24))
#define DM_TOUCH_LAYOUT(hint) ((int)((uint32_t)(hint)&0xffu))
#define DM_TOUCH_FIRST(hint)  ((int)(((uint32_t)(hint)>>8)&0xffu))
#define DM_TOUCH_SECOND(hint) ((int)(((uint32_t)(hint)>>16)&0xffu))
#define DM_TOUCH_EXTRA(hint)  ((int)(((uint32_t)(hint)>>24)&0xffu))
void dm_android_touch_hint_install(void);
uint64_t dm_android_touch_hint_get(void);
/* Feed one guest path opened for reading. Returns 1 when the hint changed. */
int dm_android_touch_hint_path(const char *path);