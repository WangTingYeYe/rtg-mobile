/* Real driver allocation/reset probe; no game resources or synthetic game frames. */
#include "../../gfx/nvn/nvn_vk_descriptor_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r);exit(1);}}while(0)
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;CHECK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t n=1;VkPhysicalDevice physical;CHECK(vkEnumeratePhysicalDevices(instance,&n,&physical));
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
    VkQueueFamilyProperties *families=calloc(count,sizeof(*families));vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;
    if(family==count)return 2;free(families);float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;CHECK(vkCreateDevice(physical,&di,NULL,&device));
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout layout;CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    NvnVkDescriptorPools pools={0};VkDescriptorSet sets[2000];
    for(unsigned i=0;i<2000;i++)CHECK(nvn_vk_descriptor_allocate(&pools,device,layout,&sets[i]));
    for(unsigned i=0;i<2000;i++)for(unsigned j=0;j<i;j++)if(sets[i]==sets[j])return 3;
    CHECK(vkDeviceWaitIdle(device));CHECK(nvn_vk_descriptor_reset(&pools,device));
    unsigned grew=pools.count;
    for(unsigned i=0;i<2000;i++)CHECK(nvn_vk_descriptor_allocate(&pools,device,layout,&sets[i]));
    if(pools.count!=grew)return 4;
    CHECK(nvn_vk_descriptor_reset(&pools,device));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16384};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=8192,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool large;CHECK(vkCreateDescriptorPool(device,&pi,NULL,&large));
    uint64_t small_ns=0,large_ns=0;
    for(unsigned round=0;round<100;round++){
        for(unsigned i=0;i<60;i++){
            CHECK(nvn_vk_descriptor_allocate(&pools,device,layout,&sets[i]));
            VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=large,.descriptorSetCount=1,.pSetLayouts=&layout};
            CHECK(vkAllocateDescriptorSets(device,&ai,&sets[100+i]));
        }
        uint64_t t=clock_ns();CHECK(nvn_vk_descriptor_reset(&pools,device));small_ns+=clock_ns()-t;
        t=clock_ns();CHECK(vkResetDescriptorPool(device,large,0));large_ns+=clock_ns()-t;
    }
    printf("PASS real driver: 2000 unique live sets, %u pools, reuse without growth; 100 resets small_ms=%.3f old8192_ms=%.3f\n",grew,small_ns/1e6,large_ns/1e6);
    for(unsigned i=0;i<pools.count;i++)vkDestroyDescriptorPool(device,pools.pools[i],NULL);
    vkDestroyDescriptorPool(device,large,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);return 0;
}
