/* Externally synchronized with the same lock as the Vulkan queue. All submit
 * fences can prove command completion; presentations still require a drain. */
#ifndef NVN_VK_QUEUE_IDLE_H
#define NVN_VK_QUEUE_IDLE_H
#include <vulkan/vulkan.h>
#include <stdint.h>
typedef struct {
    int known_idle,pending_present;
    uint64_t calls, redundant;
} NvnVkQueueIdle;
static inline VkResult nvn_vk_queue_submit(NvnVkQueueIdle *s, PFN_vkQueueSubmit fn,
        VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence fence) {
    s->known_idle=0;
    return fn(q,n,info,fence);
}
static inline VkResult nvn_vk_queue_present(NvnVkQueueIdle *s, PFN_vkQueuePresentKHR fn,
        VkQueue q, const VkPresentInfoKHR *info) {
    s->known_idle=0;s->pending_present=1;
    return fn(q,info);
}
/* Call only after every submitted command buffer's real fence AND its host
 * effects have completed, with the queue lock held. */
static inline void nvn_vk_queue_fences_complete(NvnVkQueueIdle *s) {
    if(!s->pending_present)s->known_idle=1;
}
static inline VkResult nvn_vk_queue_idle(NvnVkQueueIdle *s, PFN_vkQueueWaitIdle fn,
        VkQueue q, int reuse) {
    s->calls++;
    if(s->known_idle)s->redundant++;
    if(reuse&&s->known_idle)return VK_SUCCESS;
    VkResult result=fn(q);
    s->known_idle=result==VK_SUCCESS;
    if(result==VK_SUCCESS)s->pending_present=0;
    return result;
}
#endif
