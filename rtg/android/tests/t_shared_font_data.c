#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../host/shared_font_data.h"

/* Match the actual main+502260 type-name selection: NintendoExt requires 5.
 * A font file can contain every glyph and still fail this descriptor lookup. */
static const DmSharedFontData *find_type(const DmSharedFontData *entries,int count,unsigned type){
    for(int i=0;i<count;i++)if(entries[i].type==type)return &entries[i];
    return NULL;
}
int main(void){
    uint8_t font[64]={0};DmSharedFontData entries[6]={0};int count=-1;
    assert(dm_shared_font_data(&count,entries,6,font,sizeof font)==1);
    const DmSharedFontData *symbols=find_type(entries,count,5);
    assert(symbols&&"NintendoExt must be selectable by type 5, not font byte size");
    assert(symbols->address==font&&symbols->size==sizeof font);
    assert(count==6);
    for(unsigned type=0;type<6;type++){
        const DmSharedFontData *entry=find_type(entries,count,type);
        assert(entry&&entry->address==font&&entry->size==sizeof font&&entry->padding==0);
    }
    for(int capacity=0;capacity<=8;capacity++){
        struct {uint64_t before;DmSharedFontData data[8];uint64_t after;} guarded;
        memset(&guarded,0xa5,sizeof guarded);count=-1;
        assert(dm_shared_font_data(&count,guarded.data,capacity,font,sizeof font)==1&&count==6);
        assert(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5)&&guarded.after==guarded.before);
        int copied=capacity<6?capacity:6;
        for(int i=0;i<copied;i++)assert(guarded.data[i].type==(uint32_t)i&&guarded.data[i].address==font&&guarded.data[i].size==64);
        for(size_t i=(size_t)copied*sizeof(DmSharedFontData);i<sizeof guarded.data;i++)assert(((uint8_t *)guarded.data)[i]==0xa5);
    }
    count=-1;assert(dm_shared_font_data(&count,NULL,0,font,sizeof font)==1&&count==6);
    count=-1;assert(dm_shared_font_data(&count,entries,6,NULL,0)==0&&count==-1);
    assert(dm_shared_font_data(NULL,entries,6,font,sizeof font)==0);
    assert(dm_shared_font_data(&count,NULL,1,font,sizeof font)==0);
    puts("SHARED_FONT_ABI descriptors=6 stride=24 NintendoExt_type=5 capacity_canaries=1 query=1 passed=1");
}
