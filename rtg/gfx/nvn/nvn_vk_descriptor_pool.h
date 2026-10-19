#pragma once
#include <vulkan/vulkan.h>

/* Submission-owned pools grow without resetting live sets. The caller resets
 * only after its fence/publication. Small passes avoid the driver's 8192-set
 * pool scan; larger workloads retain automatic, bounded growth. */
typedef struct {
    VkDescriptorPool pools[8];
    unsigned count, current, used;
} NvnVkDescriptorPools;

static inline VkResult nvn_vk_descriptor_allocate(NvnVkDescriptorPools *p,
        VkDevice device, VkDescriptorSetLayout layout, VkDescriptorSet *set) {
    while (p->current < 8) {
        if (p->current == p->count) {
            uint32_t sets = 128u << p->current;
            VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets*2},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets*2},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets/2},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets*2},
                {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, sets/2},
                {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, sets/2}};
            VkDescriptorPoolCreateInfo info = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets=sets,.poolSizeCount=7,.pPoolSizes=sizes};
            VkResult r=vkCreateDescriptorPool(device,&info,NULL,&p->pools[p->count]);
            if(r!=VK_SUCCESS)return r;
            p->count++;
        }
        p->used=p->current+1;
        VkDescriptorSetAllocateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool=p->pools[p->current],.descriptorSetCount=1,.pSetLayouts=&layout};
        VkResult r=vkAllocateDescriptorSets(device,&info,set);
        if(r!=VK_ERROR_OUT_OF_POOL_MEMORY && r!=VK_ERROR_FRAGMENTED_POOL)return r;
        p->current++;
    }
    return VK_ERROR_OUT_OF_POOL_MEMORY;
}
static inline VkResult nvn_vk_descriptor_reset(NvnVkDescriptorPools *p,VkDevice device) {
    for(unsigned i=0;i<p->used;i++){
        VkResult r=vkResetDescriptorPool(device,p->pools[i],0);
        if(r!=VK_SUCCESS)return r;
    }
    p->current=p->used=0;
    return VK_SUCCESS;
}
