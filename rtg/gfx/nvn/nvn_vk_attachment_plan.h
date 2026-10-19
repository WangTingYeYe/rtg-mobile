#pragma once
#include "nvn_obj.h"

/* Port of the upstream sdx subresource/first-future-use rule (nvn_mtl.m,
 * v1.0.17): only a proven full clear before a read makes an old store dead.
 * Vulkan must choose storeOp at BEGIN, unlike Metal's mutable end-of-pass
 * store action. Stop at the very next draw/dispatch or observable operation;
 * no shader-specific assumptions, cross-submit ledger or trial discards. */
typedef struct { uint64_t serial;unsigned level,layer,plane; } NvnAttachmentKey;
typedef struct {
    uint64_t clear_loads,inline_clears,transfer_clears,discard_stores;
    /* Render-area pixels times format bytes, not physical GPU bus counters. */
    uint64_t load_bytes_avoided,store_bytes_avoided;
} NvnAttachmentStats;
static inline NvnAttachmentKey nvn_attachment_key(const tex_t *t,const view_t *v,unsigned plane) {
    return (NvnAttachmentKey){t?t->serial:0,v&&v->set_levels?(unsigned)v->base_level:0,
        v&&v->set_layers?(unsigned)v->min_layer:0,plane};
}
static inline int nvn_attachment_equal(NvnAttachmentKey a,NvnAttachmentKey b) {
    return a.serial && a.serial==b.serial && a.level==b.level && a.layer==b.layer && a.plane==b.plane;
}
static inline int nvn_clear_full(const tex_t *t,const view_t *v,p_scissor sc,int sc_set) {
    if(!t || t->d.target==NVN_TEXTURE_TARGET_3D)return 0;
    unsigned level=v&&v->set_levels?(unsigned)v->base_level:0;
    if(level>=t->d.levels || level>=32 || (v&&v->set_layers&&v->nlayers!=1))return 0;
    unsigned w=t->d.width>>level,h=t->d.height>>level;if(!w)w=1;if(!h)h=1;
    return !sc_set || (sc.x==0 && sc.y==0 && sc.w>0 && sc.h>0 && (unsigned)sc.w>=w && (unsigned)sc.h>=h);
}
/* State-only commands can be crossed by a pending clear. Every other command
 * is an observation boundary unless explicitly handled by the replay layer. */
static inline int nvn_attachment_state_op(uint32_t op) {
    switch(op){
    case OP_BEGIN:case OP_END:case OP_CALL:case OP_JUMP:
    case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:case OP_BIND_PROGRAM:
    case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
    case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:
    case OP_STENCIL:case OP_POLY_OFFSET:case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:
    case OP_COPY_STRIDE:case OP_BLEND_COLOR:case OP_BIND_IMG:case OP_SHADER_SCRATCH:return 1;
    default:return 0;
    }
}
static inline int nvn_store_dead(const uint8_t *const *ops,size_t count,size_t start,
                                p_set_rt rt,p_scissor sc,int sc_set,NvnAttachmentKey target) {
    /* Limit translation overhead; an unproven candidate always stores. */
    if(start>count)return 0;
    size_t end=count-start>256?start+256:count;
    for(size_t i=start;i<end;i++){
        op_hdr h;memcpy(&h,ops[i],sizeof h);const void *a=ops[i]+sizeof h;
        if(h.op==OP_SET_RT){if(h.size<8+sizeof(rt))return 0;memcpy(&rt,a,sizeof rt);if(rt.n<0||rt.n>8)return 0;}
        else if(h.op==OP_SCISSOR){if(h.size<8+sizeof(sc))return 0;memcpy(&sc,a,sizeof sc);sc_set=1;}
        else if(h.op==OP_CLEAR_COLOR){
            if(h.size<8+sizeof(p_clear_color))return 0;
            const p_clear_color *c=a;
            if(c->index<0||c->index>=rt.n||c->index>=8||!rt.color[c->index])return 0;
            const tex_t *t=rt.color[c->index];const view_t *v=rt.has_cview[c->index]?&rt.cview[c->index]:NULL;
            if(nvn_attachment_equal(target,nvn_attachment_key(t,v,0)))return c->mask==15&&nvn_clear_full(t,v,sc,sc_set);
        }else if(h.op==OP_CLEAR_DS){
            if(h.size<8+sizeof(p_clear_ds))return 0;
            const p_clear_ds *c=a;const view_t *v=rt.has_dview?&rt.dview:NULL;
            if(nvn_attachment_equal(target,nvn_attachment_key(rt.depth,v,target.plane)) && target.plane){
                int writes=target.plane==1?c->depth_mask!=0:(c->stencil_mask==255||c->stencil_mask==-1);
                if(writes)return nvn_clear_full(rt.depth,v,sc,sc_set);
            }
        }else if(h.op!=OP_BARRIER && !nvn_attachment_state_op(h.op))return 0;
    }
    return 0;
}
