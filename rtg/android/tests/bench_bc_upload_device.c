/* Compare decoder output paths in the same real mapped Vulkan memory type
 * selected by the renderer; CPU timing excludes the bit-exact verification. */
#include <vulkan/vulkan.h>
#include "../../gfx/tex/nvn_bcdec.h"
#include "../../third_party/bcdec/bcdec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r);exit(1);}}while(0)
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv) {
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
    VkDevice device;CHECK(vkCreateDevice(physical,&di,NULL,&device));
    const unsigned w=argc>1?3840:2048,h=argc>1?3712:2048;size_t bytes=(size_t)w*h*4,pitch=(w/4)*16;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer buffer;CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,buffer,&mr);
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    unsigned mt=0;VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while(mt<mp.memoryTypeCount&&(!(mr.memoryTypeBits&(1u<<mt))||(mp.memoryTypes[mt].propertyFlags&flags)!=flags))mt++;
    assert(mt<mp.memoryTypeCount);printf("memory_type=%u properties=%#x bytes=%zu\n",mt,mp.memoryTypes[mt].propertyFlags,bytes);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};
    VkDeviceMemory memory;CHECK(vkAllocateMemory(device,&mi,NULL,&memory));CHECK(vkBindBufferMemory(device,buffer,memory,0));
    uint8_t *mapped;CHECK(vkMapMemory(device,memory,0,bytes,0,(void **)&mapped));
    uint8_t *src=malloc(pitch*(h/4)),*reference=malloc(bytes);assert(src&&reference);uint32_t seed=1;
    for(size_t i=0;i<pitch*(h/4);i++){seed=seed*1664525+1013904223;src[i]=(uint8_t)(seed>>24);}
    if(argc>1){FILE *file=fopen(argv[1],"rb");assert(file);assert(fread(src,1,pitch*(h/4),file)==pitch*(h/4));fclose(file);}
    uint8_t *heap=malloc(bytes),*strip=malloc(w*4*4);assert(heap&&strip);
    for(unsigned y=0;y<h/4;y++)for(unsigned x=0;x<w/4;x++)bcdec_bc7(src+y*pitch+x*16,reference+(y*4*w+x*4)*4,w*4);
    for(unsigned round=0;round<3;round++)for(unsigned order=0;order<6;order++){
        unsigned path=(order+round)%3,cached=(order/3),kind=path+cached*3;uint8_t *target=cached?heap:mapped;
        uint64_t started=ns();
        if(path==0)for(unsigned y=0;y<h/4;y++)for(unsigned x=0;x<w/4;x++)bcdec_bc7(src+y*pitch+x*16,target+(y*4*w+x*4)*4,w*4);
        else if(path==1)assert(!nvn_bc_decode(0x4d,src,pitch,w,h,target,w*4));
        else for(unsigned y=0;y<h;y+=4){assert(!nvn_bc_decode(0x4d,src+(y/4)*pitch,pitch,w,4,strip,w*4));memcpy(target+(size_t)y*w*4,strip,w*4*4);}
        double ms=(ns()-started)/1e6;assert(!memcmp(reference,target,bytes));
        printf("kind=%u (reference=0 fast=1 strip=2 +cached*3) round=%u ms=%.3f pixels_identical=1\n",kind,round,ms);fflush(stdout);
    }
    free(heap);free(strip);
    free(src);free(reference);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);return 0;
}
