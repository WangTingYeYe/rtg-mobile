#pragma once
#include "../gfx/nvn/nvn_obj.h"
#include "nvn_translation_queue.h"
#include "nvn_completion_wait.h"
extern DmNvnCompletionWait dm_nvn_sync_completion;

/* A submit that can communicate with the guest CPU stays asynchronous. In
 * particular, a fence followed by a wait may need this caller to signal it. */
static inline int dm_nvn_commands_can_inline(const uint8_t *p,unsigned depth) {
    if(depth>32)return 0;
    for(unsigned guard=0;p&&guard<1000000;guard++) {
        op_hdr h;memcpy(&h,p,sizeof h);
        if(h.size<sizeof h||h.size%8||h.op>=OP_COUNT_)return 0;
        if(h.op==OP_END)return 1;
        if(h.op==OP_WAIT_EVENT||h.op==OP_SIGNAL_EVENT||h.op==OP_WAIT_SYNC||h.op==OP_FENCE_SYNC)return 0;
        if(h.op==OP_CALL||h.op==OP_JUMP) {
            if(h.size<16)return 0;
            const uint8_t *next;memcpy(&next,p+sizeof h,sizeof next);
            if(h.op==OP_JUMP){p=next;continue;}
            if(!dm_nvn_commands_can_inline(next,depth+1))return 0;
        }
        p+=h.size;
    }
    return 0;
}

/* Deliberately narrow: these records only update the replay's persistent CPU
 * bindings. Barriers must be carried to the next real GPU command buffer.
 * Never classify a draw, resource operation, event, query or fence this way. */
static inline int dm_nvn_cpu_state_op(uint32_t op) {
    switch(op) {
    case OP_END:case OP_BEGIN:case OP_CALL:case OP_JUMP:
    case OP_BIND_VBUF:case OP_BARRIER:case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:
    case OP_SHADER_SCRATCH:case OP_BIND_STATE:case OP_POLY_OFFSET:case OP_STENCIL:
    case OP_BIND_VATTR:case OP_BIND_VSTREAM:return 1;
    default:return 0;
    }
}
static inline int dm_nvn_commands_cpu_state(const uint8_t *p,unsigned depth) {
    if(depth>32)return 0;
    for(unsigned guard=0;p&&guard<1000000;guard++) {
        op_hdr h;memcpy(&h,p,sizeof h);
        if(h.size<sizeof h||h.size%8||!dm_nvn_cpu_state_op(h.op))return 0;
        if(h.op==OP_END)return 1;
        if(h.op==OP_CALL||h.op==OP_JUMP) {
            if(h.size<16)return 0;
            const uint8_t *next;memcpy(&next,p+sizeof h,sizeof next);
            if(h.op==OP_JUMP){p=next;continue;}
            if(!dm_nvn_commands_cpu_state(next,depth+1))return 0;
        }
        p+=h.size;
    }
    return 0;
}

/* Only structural records may bypass Vulkan. State changes, barriers, fences,
 * queries and events still take the normal replay path, even without draws.
 * Preparation has already validated headers and marked all recorded fences.
 * Keep the same recursion/iteration limits as replay; uncertain input is not
 * classified as empty. An empty job still completes in CPU queue order, so a
 * following queue fence refers to the preceding real backend submission. */
static inline int dm_nvn_commands_empty(const uint8_t *p,unsigned depth) {
    if(depth>32)return 0;
    for(unsigned guard=0;p && guard<1000000;guard++) {
        op_hdr h;memcpy(&h,p,sizeof h);
        if(h.size<sizeof h || h.size%8)return 0;
        if(h.op==OP_END)return 1;
        if(h.op==OP_CALL || h.op==OP_JUMP) {
            if(h.size<16)return 0;
            const uint8_t *next;memcpy(&next,p+sizeof h,sizeof next);
            if(h.op==OP_JUMP){p=next;continue;}
            if(!dm_nvn_commands_empty(next,depth+1))return 0;
        }else if(h.op!=OP_BEGIN)return 0;
        p+=h.size;
    }
    return 0;
}

/* Mark fences before replay can start. Walk only the host-generated record
 * headers, including CALL/JUMP; command payloads/resources remain in guest
 * memory under NVN's normal until-completion lifetime contract. No allocation
 * or Vulkan calls here. Bound malformed/cyclic streams just as replay does. */
static inline int dm_nvn_mark_fences(const uint8_t *p,unsigned depth) {
    if(depth>32)return 0;
    for(unsigned guard=0;p && guard<1000000;guard++) {
        op_hdr h;memcpy(&h,p,sizeof h);
        if(h.size<sizeof h || h.size%8 || h.op>=OP_COUNT_)return 0;
        const uint8_t *a=p+sizeof h;
        if(h.op==OP_END)return 1;
        if(h.op==OP_CALL || h.op==OP_JUMP) {
            if(h.size<16)return 0;
            const uint8_t *next;memcpy(&next,a,sizeof next);
            if(h.op==OP_JUMP){p=next;continue;}
            if(!dm_nvn_mark_fences(next,depth+1))return 0;
        } else if(h.op==OP_FENCE_SYNC) {
            if(h.size<sizeof h+sizeof(p_sync))return 0;
            p_sync fence;memcpy(&fence,a,sizeof fence);
            if(!fence.sync || fence.sync->magic!=MAGIC('S','Y','N','C'))return 0;
            atomic_fetch_add(&fence.sync->pending,1);
        }
        p+=h.size;
    }
    return 0;
}
static inline void dm_nvn_sync_publish(sync_t *s,uint64_t fence) {
    /* Direct backend probes do not go through the host translation queue. */
    if(!dm_nvn_translation_current_ticket())atomic_fetch_add(&s->pending,1);
    atomic_store(&s->fence,fence);
    atomic_store(&s->fenced,1);
    atomic_fetch_add(&s->translated,1);
    dm_nvn_completion_notify(&dm_nvn_sync_completion);
}
