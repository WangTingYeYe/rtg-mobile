/* Exercise the APK's actual pool registry and Vulkan device, without saves. */
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
int main(int argc,char **argv) {
    assert(argc>=2);
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
    void (*add)(const void *,uint64_t)=dlsym(lib,"mtl_pool_add");
    void (*del)(const void *)=dlsym(lib,"mtl_pool_del");
    int (*find)(uint64_t,uint64_t,void **,uint64_t *)=dlsym(lib,"mtl_addr");
    void (*begin)(void)=dlsym(lib,"mtl_sub_begin");
    void (*copy)(uint64_t,uint64_t,uint64_t)=dlsym(lib,"mtl_sub_copy");
    void (*commit_async)(void)=dlsym(lib,"mtl_sub_commit_async");
    void (*finish)(void)=dlsym(lib,"mtl_finish");
    assert(add&&del&&find&&begin&&copy&&commit_async&&finish);
    unsigned count=argc>2?(unsigned)atoi(argv[2]):1025;
    unsigned rounds=argc>3?(unsigned)atoi(argv[3]):20;
    uint8_t *memory=mmap(NULL,(size_t)count*8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);
    setbuf(stdout,NULL);
    for(unsigned round=0;round<rounds;round++){
        for(unsigned b=0;b<4096;b++)memory[b]=(uint8_t)(b+round);
        for(unsigned i=0;i<count;i++){
            if(round==0&&i==512)puts("REPRO registering 513th valid 4096-byte memory pool");
            add(memory+(size_t)i*8192,4096);
            /* Preserve real GPU backing and pending guest writeback while the
             * registry grows at 513/1025, not just unused registry entries. */
            if(i==511){begin();copy((uintptr_t)memory,(uintptr_t)memory+8192,4096);commit_async();}
        }
        if(count>512){finish();assert(!memcmp(memory,memory+8192,4096));}
        for(unsigned i=0;i<count;i++){
            void *pool=NULL;uint64_t offset=0;
            assert(find((uintptr_t)memory+(size_t)i*8192+16,4080,&pool,&offset));
            assert(pool&&offset==16);
            assert(!find((uintptr_t)memory+(size_t)i*8192+16,4081,&pool,&offset));
        }
        /* Delete in interleaved order to exercise compaction and address reuse. */
        for(unsigned parity=0;parity<2;parity++)for(unsigned i=parity;i<count;i+=2){
            del(memory+(size_t)i*8192);void *pool=NULL;uint64_t offset=0;
            assert(!find((uintptr_t)memory+(size_t)i*8192,1,&pool,&offset));
        }
    }
    munmap(memory,(size_t)count*8192);
    printf("PASS %u live pools, %u complete create/find/delete/reuse rounds\n",count,rounds);
    return 0;
}
