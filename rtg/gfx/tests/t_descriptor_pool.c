#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../nvn/nvn_vk_descriptor_pool.h"
static unsigned created,resets,capacity[8],allocated[8];
static VkResult next_error=VK_SUCCESS;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *p){
    (void)d;(void)a;assert(created<8&&i->poolSizeCount==7);capacity[created]=i->maxSets;
    *p=(VkDescriptorPool)(uintptr_t)(++created);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *s){
    (void)d;unsigned p=(unsigned)(uintptr_t)i->descriptorPool-1;assert(p<created&&i->descriptorSetCount==1);
    if(next_error!=VK_SUCCESS){VkResult e=next_error;next_error=VK_SUCCESS;return e;}
    if(allocated[p]==capacity[p])return VK_ERROR_OUT_OF_POOL_MEMORY;
    *s=(VkDescriptorSet)(uintptr_t)(((p+1)<<20)|++allocated[p]);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){
    (void)d;(void)f;unsigned index=(unsigned)(uintptr_t)p-1;assert(index<created);allocated[index]=0;resets++;return VK_SUCCESS;
}
int main(void){
    NvnVkDescriptorPools p={0};VkDescriptorSet sets[2000];
    assert(nvn_vk_descriptor_reset(&p,VK_NULL_HANDLE)==VK_SUCCESS&&resets==0);
    for(unsigned i=0;i<2000;i++)assert(nvn_vk_descriptor_allocate(&p,VK_NULL_HANDLE,VK_NULL_HANDLE,&sets[i])==VK_SUCCESS);
    assert(created==5&&resets==0&&capacity[0]==128);
    for(unsigned i=0;i<2000;i++)for(unsigned j=0;j<i;j++)assert(sets[i]!=sets[j]);
    unsigned count=created;assert(nvn_vk_descriptor_reset(&p,VK_NULL_HANDLE)==VK_SUCCESS&&resets==count);
    assert(nvn_vk_descriptor_allocate(&p,VK_NULL_HANDLE,VK_NULL_HANDLE,&sets[0])==VK_SUCCESS&&created==count);
    next_error=VK_ERROR_DEVICE_LOST;
    assert(nvn_vk_descriptor_allocate(&p,VK_NULL_HANDLE,VK_NULL_HANDLE,&sets[1])==VK_ERROR_DEVICE_LOST&&created==count);
    next_error=VK_ERROR_FRAGMENTED_POOL;
    assert(nvn_vk_descriptor_allocate(&p,VK_NULL_HANDLE,VK_NULL_HANDLE,&sets[1])==VK_SUCCESS&&created==count);
    assert(p.current==1&&p.used==2);
    unsigned before=resets;assert(nvn_vk_descriptor_reset(&p,VK_NULL_HANDLE)==VK_SUCCESS&&resets==before+2);
    puts("PASS: 2000 live unique sets, overflow growth, fragmentation, fence-scoped reuse, real errors retained");
}
