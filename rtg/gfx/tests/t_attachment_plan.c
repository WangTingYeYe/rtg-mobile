/* Compare the production store proof with a pixel/plane reference model.
 * A discarded attachment starts poisoned; no observation may distinguish it
 * from the stored version when the proof permits DONT_CARE. No GPU is used. */
#include <stdio.h>
#include <stdlib.h>
#include "../nvn/nvn_vk_attachment_plan.h"

#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"attachment check line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
typedef struct { op_hdr h; union { p_set_rt rt;p_scissor sc;p_clear_color color;p_clear_ds ds; } p; } Record;
static Record records[300];static const uint8_t *ops[300];static unsigned count,checks,proofs;
static tex_t tex[2];static p_set_rt initial;static unsigned rng=0x71356abc;
static unsigned random_u(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void add(unsigned op,const void *payload,size_t size){
    Record *r=&records[count];memset(r,0,sizeof *r);r->h=(op_hdr){op,(uint32_t)((8+size+7)&~7u)};
    if(size)memcpy(&r->p,payload,size);ops[count++]=(const uint8_t *)r;
}
static void color(int mask){p_clear_color c={.mask=mask};add(OP_CLEAR_COLOR,&c,sizeof c);}
static int dead(unsigned plane){return nvn_store_dead(ops,count,0,initial,(p_scissor){0},0,nvn_attachment_key(&tex[0],NULL,plane));}
static void select_rt(unsigned texture,unsigned mip,unsigned layer){
    p_set_rt rt=initial;rt.color[0]=rt.depth=&tex[texture];rt.has_cview[0]=rt.has_dview=1;
    rt.cview[0]=rt.dview=(view_t){.set_levels=1,.set_layers=1,.base_level=(int)mip,.nlevels=1,.min_layer=(int)layer,.nlayers=1};
    add(OP_SET_RT,&rt,sizeof rt);
}
/* Model only the target mip 0/layer 0. Other subresources are independent.
 * Each pixel retains a bit mask of the difference between stored and poisoned
 * initial data. A later clear removes precisely the bits it overwrites. */
static int observable_difference(unsigned plane){
    unsigned difference[16];for(unsigned i=0;i<16;i++)difference[i]=plane==0?15:255;
    p_set_rt rt=initial;p_scissor sc={0,0,4,4};
    for(unsigned i=0;i<count;i++){
        const Record *r=&records[i];unsigned mask=0;const tex_t *t=NULL;const view_t *v=NULL;
        switch(r->h.op){
        case OP_SET_RT:rt=r->p.rt;continue;
        case OP_SCISSOR:sc=r->p.sc;continue;
        case OP_CLEAR_COLOR:
            if(plane!=0)continue;
            t=rt.color[0];v=rt.has_cview[0]?&rt.cview[0]:NULL;mask=(unsigned)r->p.color.mask;break;
        case OP_CLEAR_DS:
            if(plane==0)continue;
            t=rt.depth;v=rt.has_dview?&rt.dview:NULL;
            mask=plane==1?(r->p.ds.depth_mask?255:0):(unsigned)r->p.ds.stencil_mask;break;
        case OP_BIND_STATE:case OP_BIND_TEX:case OP_BARRIER:case OP_BEGIN:case OP_END:case OP_CALL:case OP_JUMP:continue;
        default:goto observe;
        }
        if(t!=&tex[0]||(v&&((v->set_levels&&v->base_level)||(v->set_layers&&v->min_layer))))continue;
        for(int y=0;y<4;y++)for(int x=0;x<4;x++)
            if(x>=sc.x&&y>=sc.y&&x<sc.x+sc.w&&y<sc.y+sc.h)difference[y*4+x]&=~mask;
    }
observe:
    for(unsigned i=0;i<16;i++)if(difference[i])return 1;
    return 0;
}
int main(void){
    tex[0]=(tex_t){.serial=1,.d={.target=NVN_TEXTURE_TARGET_2D_ARRAY,.width=4,.height=4,.depth=2,.levels=2},.L={.layers=2}};
    tex[1]=tex[0];tex[1].serial=2;initial=(p_set_rt){.n=1,.color={&tex[0]},.depth=&tex[0]};
    color(15);CHECK(dead(0));CHECK(!dead(1));CHECK(!dead(2));
    for(unsigned texture=0;texture<2;texture++)for(unsigned mip=0;mip<2;mip++)for(unsigned layer=0;layer<2;layer++){
        count=0;select_rt(texture,mip,layer);color(15);CHECK(dead(0)==(!texture&&!mip&&!layer));
    }
    count=0;color(7);CHECK(!dead(0));
    for(int width=0;width<=5;width++)for(int x=0;x<=4;x++){
        count=0;p_scissor sc={x,0,width,4};add(OP_SCISSOR,&sc,sizeof sc);color(15);CHECK(dead(0)==(x==0&&width>=4));
    }
    unsigned boundaries[]={OP_DRAW,OP_DRAW_IDX,OP_DISPATCH,OP_COPY_T2T,OP_COPY_T2B,OP_COPY_B2T,OP_COPY_B2B,
        OP_CLEAR_TEX,OP_UPDATE_UB,OP_WAIT_EVENT,OP_SIGNAL_EVENT,OP_WAIT_SYNC,OP_FENCE_SYNC,OP_REPORT_COUNTER,OP_RESET_COUNTER,OP_COUNT_+1};
    for(unsigned i=0;i<sizeof boundaries/sizeof *boundaries;i++){
        count=0;add(boundaries[i],NULL,0);color(15);CHECK(!dead(0));
    }
    for(int dm=0;dm<=1;dm++)for(int sm=0;sm<=255;sm+=17){
        count=0;p_clear_ds ds={.depth_mask=dm,.stencil_mask=sm};add(OP_CLEAR_DS,&ds,sizeof ds);
        CHECK(dead(1)==dm);CHECK(dead(2)==(sm==255));
    }
    count=0;for(unsigned i=0;i<256;i++)add(OP_BIND_STATE,NULL,0);color(15);CHECK(!dead(0));
    CHECK(!nvn_store_dead(ops,count,count+1,initial,(p_scissor){0},0,nvn_attachment_key(&tex[0],NULL,0)));
    count=0;color(15);records[0].h.size=8;CHECK(!dead(0));
    view_t v={.set_levels=1,.base_level=1,.nlevels=1};
    CHECK(nvn_clear_full(&tex[0],&v,(p_scissor){0,0,2,2},1));
    v.base_level=32;CHECK(!nvn_clear_full(&tex[0],&v,(p_scissor){0},0));
    v.base_level=-1;CHECK(!nvn_clear_full(&tex[0],&v,(p_scissor){0},0));
    v=(view_t){.set_layers=1,.nlayers=2};CHECK(!nvn_clear_full(&tex[0],&v,(p_scissor){0},0));
    tex[0].d.target=NVN_TEXTURE_TARGET_3D;CHECK(!nvn_clear_full(&tex[0],NULL,(p_scissor){0},0));tex[0].d.target=NVN_TEXTURE_TARGET_2D_ARRAY;
    CHECK(!nvn_attachment_equal((NvnAttachmentKey){0},(NvnAttachmentKey){0}));
    for(unsigned trial=0;trial<20000;trial++){
        count=0;
        for(unsigned i=0;i<20;i++)switch(random_u()%10){
        case 0:select_rt(random_u()%2,random_u()%2,random_u()%2);break;
        case 1:{p_scissor sc={(int)(random_u()%3),0,(int)(random_u()%6),4};add(OP_SCISSOR,&sc,sizeof sc);break;}
        case 2:color(15);break;
        case 3:color(7);break;
        case 4:{p_clear_ds ds={.depth_mask=(int)(random_u()%2),.stencil_mask=random_u()%2?255:15};add(OP_CLEAR_DS,&ds,sizeof ds);break;}
        case 5:add(OP_BIND_TEX,NULL,0);break;
        case 6:add(OP_BARRIER,NULL,0);break;
        case 7:add(OP_DRAW,NULL,0);break;
        case 8:add(OP_COPY_T2T,NULL,0);break;
        case 9:add(OP_FENCE_SYNC,NULL,0);break;
        }
        for(unsigned plane=0;plane<3;plane++)if(dead(plane)){proofs++;CHECK(!observable_difference(plane));}
    }
    CHECK(proofs>1000);
    printf("ATTACHMENT_POLICY checks=%u randomized_streams=20000 safe_discards=%u passed=1\n",checks,proofs);
}
