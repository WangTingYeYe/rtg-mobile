#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn_bc7_tables.h"
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
#include <arm_neon.h>
#endif

static inline uint32_t bc7_field(uint64_t low,uint64_t high,unsigned offset,unsigned bits) {
    uint64_t value=offset>=64?high>>(offset-64):low>>offset;
    if(offset<64&&offset+bits>64)value|=high<<(64-offset);
    return (uint32_t)value&((1u<<bits)-1);
}
static inline uint32_t bc7_rgba(unsigned r,unsigned g,unsigned b,unsigned a) {
    return r|(g<<8)|(b<<16)|(a<<24);
}
static inline void bc7_palette(uint32_t a,uint32_t b,unsigned bits,uint32_t *palette) {
    static const uint16_t weights2[4]={0,21,43,64};
    static const uint16_t weights4[16]={0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
    unsigned count=1u<<bits;const uint16_t *weights=bits==2?weights2:weights4;
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
    uint16x8_t left=vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(a)));
    uint16x8_t right=vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(b)));
    uint16x8_t base=vshlq_n_u16(left,6),difference=vsubq_u16(right,left);
    for(unsigned i=0;i<count;i+=2){
        uint16x8_t w=vcombine_u16(vdup_n_u16(weights[i]),vdup_n_u16(weights[i+1]));
        uint8x8_t colors=vrshrn_n_u16(vmlaq_u16(base,difference,w),6);
        vst1_u8((uint8_t *)(palette+i),colors);
    }
#else
    for(unsigned i=0;i<count;i++){
        uint32_t out=0,w=weights[i];
        for(unsigned c=0;c<4;c++)out|=((((a>>(c*8))&255)*(64-w)+((b>>(c*8))&255)*w+32)>>6)<<(c*8);
        palette[i]=out;
    }
#endif
}
static inline uint32_t bc7_rotate(uint32_t pixel,unsigned rotation) {
    if(!rotation)return pixel;
    unsigned shift=(rotation-1)*8;
    uint32_t alpha=pixel>>24,channel=(pixel>>shift)&255;
    return (pixel&~(0xff000000u|(255u<<shift)))|(alpha<<shift)|(channel<<24);
}
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
static inline uint8x16_t bc7_indices2(uint32_t bits) {
    /* Restore pixel zero's implicit high bit, then expand sixteen indices. */
    bits=(bits&1)|((bits&~1u)<<1);
    const uint8x16_t bytes={0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3};
    const int8x16_t shifts={0,-2,-4,-6,0,-2,-4,-6,0,-2,-4,-6,0,-2,-4,-6};
    return vandq_u8(vshlq_u8(vqtbl1q_u8(vreinterpretq_u8_u32(vdupq_n_u32(bits)),bytes),shifts),vdupq_n_u8(3));
}
static inline uint8x16_t bc7_pixel_lookup(uint8x16_t indices,unsigned row) {
    const uint8x16_t replicate={0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3};
    const uint8x16_t channels={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3};
    return vaddq_u8(vshlq_n_u8(vqtbl1q_u8(indices,vaddq_u8(replicate,vdupq_n_u8(row*4))),2),channels);
}
#endif
/* Common atlas modes use explicit fields and a small, vectorized palette.
 * Returns zero for other modes so the full reference decoder stays available. */
static inline int nvn_bc7_fast_block(const uint8_t *src,uint8_t *dst,size_t pitch) {
    unsigned first=src[0],mode=first?(unsigned)__builtin_ctz(first):8;
    if(mode!=3&&mode!=5&&mode!=6)return 0;
    uint64_t low,high;memcpy(&low,src,8);memcpy(&high,src+8,8);
    uint32_t palette[16];
    if(mode==6){
        unsigned p0=(unsigned)(low>>63),p1=(unsigned)high&1;
        uint32_t a=bc7_rgba((bc7_field(low,high,7,7)<<1)|p0,(bc7_field(low,high,21,7)<<1)|p0,
            (bc7_field(low,high,35,7)<<1)|p0,(bc7_field(low,high,49,7)<<1)|p0);
        uint32_t b=bc7_rgba((bc7_field(low,high,14,7)<<1)|p1,(bc7_field(low,high,28,7)<<1)|p1,
            (bc7_field(low,high,42,7)<<1)|p1,(bc7_field(low,high,56,7)<<1)|p1);
        bc7_palette(a,b,4,palette);uint64_t indices=high>>1;
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
        uint8x8_t packed=vcreate_u8((indices&7)|((indices&~7ull)<<1));
        uint8x8x2_t pairs=vzip_u8(vand_u8(packed,vdup_n_u8(15)),vshr_n_u8(packed,4));
        uint8x16_t ix=vcombine_u8(pairs.val[0],pairs.val[1]);
        uint8x16x4_t colors=vld1q_u8_x4((const uint8_t *)palette);
        for(unsigned row=0;row<4;row++)vst1q_u8(dst+row*pitch,vqtbl4q_u8(colors,bc7_pixel_lookup(ix,row)));
#else
        uint32_t pixel=palette[indices&7];memcpy(dst,&pixel,4);indices>>=3;
        for(unsigned i=1;i<16;i++){pixel=palette[indices&15];memcpy(dst+(i/4)*pitch+(i%4)*4,&pixel,4);indices>>=4;}
#endif
    }else if(mode==5){
        uint32_t endpoints[2]={0};
        for(unsigned e=0;e<2;e++){
            unsigned r=bc7_field(low,high,8+7*e,7),g=bc7_field(low,high,22+7*e,7),b=bc7_field(low,high,36+7*e,7);
            endpoints[e]=bc7_rgba((r<<1)|(r>>6),(g<<1)|(g>>6),(b<<1)|(b>>6),bc7_field(low,high,50+8*e,8));
        }
        bc7_palette(endpoints[0],endpoints[1],2,palette);
        uint32_t colors=(uint32_t)(high>>2),alphas=(uint32_t)(high>>33);unsigned rotation=(low>>6)&3;
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
        uint8x16_t ci=bc7_indices2(colors),ai=bc7_indices2(alphas),table=vld1q_u8((const uint8_t *)palette);
        static const uint8_t rotations[4][16]={
            {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
            {3,1,2,0,7,5,6,4,11,9,10,8,15,13,14,12},
            {0,3,2,1,4,7,6,5,8,11,10,9,12,15,14,13},
            {0,1,3,2,4,5,7,6,8,9,11,10,12,13,15,14}};
        uint8x16_t permutation=vld1q_u8(rotations[rotation]);
        uint8x16_t alpha_mask=vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));
        for(unsigned row=0;row<4;row++){
            uint8x16_t rgb=vqtbl1q_u8(table,bc7_pixel_lookup(ci,row)),alpha=vqtbl1q_u8(table,bc7_pixel_lookup(ai,row));
            vst1q_u8(dst+row*pitch,vqtbl1q_u8(vbslq_u8(alpha_mask,alpha,rgb),permutation));
        }
#else
        for(unsigned i=0;i<16;i++){
            unsigned mask=i?3:1,shift=i?2:1;
            uint32_t pixel=bc7_rotate((palette[colors&mask]&0x00ffffffu)|(palette[alphas&mask]&0xff000000u),rotation);
            memcpy(dst+(i/4)*pitch+(i%4)*4,&pixel,4);colors>>=shift;alphas>>=shift;
        }
#endif
    }else{
        uint32_t endpoints[4];unsigned partition=(low>>4)&63;
        for(unsigned e=0;e<4;e++){
            unsigned p=(high>>(30+e))&1;
            endpoints[e]=bc7_rgba((bc7_field(low,high,10+7*e,7)<<1)|p,
                (bc7_field(low,high,38+7*e,7)<<1)|p,(bc7_field(low,high,66+7*e,7)<<1)|p,255);
        }
        bc7_palette(endpoints[0],endpoints[1],2,palette);bc7_palette(endpoints[2],endpoints[3],2,palette+4);
        uint64_t indices=high>>34;
#if defined(__aarch64__) && !defined(NVN_BC7_SCALAR)
        /* Insert the second subset's omitted high index bit before the shared
         * two-bit unpacker restores pixel zero's omitted bit. */
        unsigned anchor=(unsigned)__builtin_ctz(nvn_bc7_tables[512+partition]&~1u);
        unsigned hole=anchor*2;uint32_t lower=(1u<<hole)-1;
        uint32_t bits=((uint32_t)indices&lower)|(((uint32_t)indices&~lower)<<1);
        uint8x16_t subsets=vandq_u8(vld1q_u8((const uint8_t *)(nvn_bc7_tables+partition*4)),vdupq_n_u8(1));
        uint8x16_t ix=vorrq_u8(bc7_indices2(bits),vshlq_n_u8(subsets,2));
        uint8x16x2_t colors=vld1q_u8_x2((const uint8_t *)palette);
        for(unsigned row=0;row<4;row++)vst1q_u8(dst+row*pitch,vqtbl2q_u8(colors,bc7_pixel_lookup(ix,row)));
#else
        for(unsigned i=0;i<16;i++){
            unsigned cell=(nvn_bc7_tables[partition*4+i/4]>>((i%4)*8))&255;
            unsigned bits=2-((cell>>7)&1),index=indices&((1u<<bits)-1);indices>>=bits;
            uint32_t pixel=palette[(cell&1)*4+index];memcpy(dst+(i/4)*pitch+(i%4)*4,&pixel,4);
        }
#endif
    }
    return 1;
}
