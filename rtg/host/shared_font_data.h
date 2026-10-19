#ifndef DM_SHARED_FONT_DATA_H
#define DM_SHARED_FONT_DATA_H
#include <stddef.h>
#include <stdint.h>

/* nn::pl::SharedFontData as consumed by the shipped RTG SDK/game. */
typedef struct {
    uint32_t type;
    uint32_t padding;
    const void *address;
    uint64_t size;
} DmSharedFontData;
_Static_assert(sizeof(DmSharedFontData)==24,"SharedFontData ABI size");
_Static_assert(offsetof(DmSharedFontData,address)==8,"SharedFontData address offset");
_Static_assert(offsetof(DmSharedFontData,size)==16,"SharedFontData size offset");

static inline uint32_t dm_shared_font_data(int *count,void *out,int capacity,const void *font,size_t size) {
    if(!count||capacity<0||(!out&&capacity>0)||!font||!size)return 0;
    /* The SDK reports all available entries, even for a zero-capacity query.
     * Types 0..4 are language families, 5 is NintendoExt. Our complete Unicode
     * compatibility font supplies each family, with stable shared storage. */
    *count=6;
    DmSharedFontData *entries=out;
    int copied=capacity<6?capacity:6;
    for(int i=0;i<copied;i++)entries[i]=(DmSharedFontData){(uint32_t)i,0,font,size};
    return 1;
}
#endif
