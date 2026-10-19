/* Real Vulkan fence/readback regression for finish reuse, including chunks
 * submitted without a fence and covered by the final submission's fence. */
#include "../../gfx/nvn/nvn_vk_queue_idle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r);exit(1);}}while(0)
static unsigned driver_waits;
static VKAPI_ATTR VkResult VKAPI_CALL counted_wait(VkQueue q) {driver_waits++;return vkQueueWaitIdle(q);}
int main(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;CHECK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t n=1;VkPhysicalDevice physical;CHECK(vkEnumeratePhysicalDevices(instance,&n,&physical));
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
    VkQueueFamilyProperties *families=calloc(count,sizeof(*families));vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;
    assert(family<count);free(families);float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;CHECK(vkCreateDevice(physical,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,buffer,&mr);
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    unsigned mt=0;VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while(mt<mp.memoryTypeCount&&(!(mr.memoryTypeBits&(1u<<mt))||(mp.memoryTypes[mt].propertyFlags&flags)!=flags))mt++;
    assert(mt<mp.memoryTypeCount);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};
    VkDeviceMemory memory;CHECK(vkAllocateMemory(device,&mi,NULL,&memory));CHECK(vkBindBufferMemory(device,buffer,memory,0));
    uint32_t *mapped;CHECK(vkMapMemory(device,memory,0,4096,0,(void **)&mapped));
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool;CHECK(vkCreateCommandPool(device,&cp,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=2};
    VkCommandBuffer commands[2];CHECK(vkAllocateCommandBuffers(device,&ca,commands));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;CHECK(vkCreateFence(device,&fi,NULL,&fence));
    for(int reuse=0;reuse<=1;reuse++) {
        NvnVkQueueIdle state={0};driver_waits=0;
        for(unsigned round=0;round<128;round++) {
            CHECK(vkResetCommandPool(device,pool,0));CHECK(vkResetFences(device,1,&fence));
            for(unsigned chunk=0;chunk<2;chunk++) {
                VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                CHECK(vkBeginCommandBuffer(commands[chunk],&begin));
                vkCmdFillBuffer(commands[chunk],buffer,chunk*2048,2048,round*2+chunk);
                VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
                vkCmdPipelineBarrier(commands[chunk],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
                CHECK(vkEndCommandBuffer(commands[chunk]));
                VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&commands[chunk]};
                CHECK(nvn_vk_queue_submit(&state,vkQueueSubmit,queue,1,&submit,chunk?fence:VK_NULL_HANDLE));
                assert(!state.known_idle);
            }
            CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX));
            nvn_vk_queue_fences_complete(&state);
            CHECK(nvn_vk_queue_idle(&state,counted_wait,queue,reuse));
            for(unsigned word=0;word<1024;word++)assert(mapped[word]==round*2+(word>=512));
        }
        assert(driver_waits==(reuse?0:128));
        printf("PASS real GPU: reuse=%d, 128 fence-verified readbacks, 256 chunks, driver_idle_calls=%u\n",reuse,driver_waits);
    }
    CHECK(vkQueueWaitIdle(queue));vkDestroyFence(device,fence,NULL);vkDestroyCommandPool(device,pool,NULL);
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
}
