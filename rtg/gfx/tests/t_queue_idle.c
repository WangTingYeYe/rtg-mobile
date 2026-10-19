#include <assert.h>
#include <stdio.h>
#include "../nvn/nvn_vk_queue_idle.h"
static unsigned waits,submits,presents;
static VkResult wait_result=VK_SUCCESS;
static VKAPI_ATTR VkResult VKAPI_CALL wait_queue(VkQueue q) {
    (void)q;waits++;return wait_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
    (void)q;(void)n;(void)i;(void)f;submits++;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue q,const VkPresentInfoKHR *i) {
    (void)q;(void)i;presents++;return VK_SUCCESS;
}
int main(void) {
    NvnVkQueueIdle s={0};
    assert(nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1)==VK_SUCCESS);
    assert(nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1)==VK_SUCCESS);
    assert(waits==1); /* Repeated QueueFinish after no new work. */
    nvn_vk_queue_submit(&s,submit,VK_NULL_HANDLE,0,NULL,VK_NULL_HANDLE);
    assert(!s.known_idle);
    nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1);assert(waits==2);
    nvn_vk_queue_present(&s,present,VK_NULL_HANDLE,NULL);
    assert(!s.known_idle);
    wait_result=VK_ERROR_DEVICE_LOST;
    assert(nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1)==VK_ERROR_DEVICE_LOST);
    assert(nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1)==VK_ERROR_DEVICE_LOST);
    assert(waits==4&&!s.known_idle); /* Errors never turn into cached success. */
    wait_result=VK_SUCCESS;
    nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1);
    nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,0);assert(waits==6);
    assert(submits==1&&presents==1&&s.calls==7&&s.redundant==2);
    nvn_vk_queue_submit(&s,submit,VK_NULL_HANDLE,0,NULL,VK_NULL_HANDLE);
    nvn_vk_queue_fences_complete(&s);
    nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1);assert(waits==6);
    nvn_vk_queue_present(&s,present,VK_NULL_HANDLE,NULL);
    nvn_vk_queue_fences_complete(&s);
    assert(!s.known_idle); /* The blit's fence does not cover presentation. */
    nvn_vk_queue_idle(&s,wait_queue,VK_NULL_HANDLE,1);assert(waits==7);
    puts("PASS: repeated finish, submit/present invalidation, error propagation, A/B bypass");
}
