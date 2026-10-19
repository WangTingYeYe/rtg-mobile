// 节奏天国移植新增：nvnBuffer 对象家族（D2R 用内存池直取址、不经 Buffer 对象；SDK 21.4 的本游戏走 Buffer API）。
// 模型沿用本层的零拷贝约定：GPU 地址 = CPU 地址；Buffer 只是 (池, 偏移, 大小) 的薄包装，Map 返回池内存 + 偏移。
// Added for this port: the nvnBuffer object family. Same zero-copy model as the rest of this layer:
// GPU address = CPU address; a Buffer is a thin (pool, offset, size) wrapper and Map returns pool memory + offset.
#include "../../host/hle.h"
#include "nvn_protos.h"
#include "nvn_count.h"
#include "nvn_obj.h"
#include <string.h>
#include <stddef.h>

// NVN 宏与 nvn_impl 的定义在 hle_nvn.c 内（未进头文件）；此处按原样复制（fork 内后续应抽进公共头）
// The NVN macro and nvn_impl live in hle_nvn.c (not in a header); copied verbatim here
typedef struct { const char *name; void *fn; } nvn_impl_;
#ifdef __ANDROID__
#define NVN_SECTION "swnvn"     // 安卓 ELF:裸段名(hle_nvn.c 同款;__DATA 前缀不可用)
#else
#define NVN_SECTION "__DATA,swnvn"
#endif
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    __attribute__((used, aligned(8), section(NVN_SECTION))) static const nvn_impl_ e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)

typedef struct { NVNdevice *dev; const pool_t *pool; ptrdiff_t off; size_t n; } buf_builder_t;
FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; const pool_t *pool; ptrdiff_t off; size_t n; } buf_t;
FITS(buf_t, NVNbuffer);

NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { memset(b, 0, sizeof(buf_builder_t)); }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { ((buf_builder_t *)b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    buf_builder_t *o = (buf_builder_t *)b; o->pool = (const pool_t *)p; o->off = off; o->n = n;
}
NVN(nvnBufferBuilderGetMemoryPool, NVNmemoryPool, const NVNbufferBuilder *b) {
    NVNmemoryPool r; const pool_t *p = ((buf_builder_t *)b)->pool;
    memset(&r, 0, sizeof r); if (p) memcpy(&r, p, sizeof(pool_t));   // 原型按值返回：拷池头 | the prototype returns by value: copy the pool header
    return r;
}
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return ((buf_builder_t *)b)->off; }
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return ((buf_builder_t *)b)->n; }

NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    buf_builder_t *s = (buf_builder_t *)b; buf_t *o = (buf_t *)buf;
    o->magic = MAGIC('B', 'U', 'F', 'R'); o->dev = s->dev; o->pool = s->pool; o->off = s->off; o->n = s->n;
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *buf) { ((buf_t *)buf)->magic = 0; }
NVN(nvnBufferMap, void*, const NVNbuffer *buf) {
    buf_t *o = (buf_t *)buf;
    if (o->magic != MAGIC('B', 'U', 'F', 'R') || !o->pool || !o->pool->mem) { LOG("【红】nvnBufferMap 未初始化/无存储 buf=%p", buf); return NULL; }
    return o->pool->mem + o->off;
}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *buf) {
    buf_t *o = (buf_t *)buf;
    return (NVNbufferAddress)(o->pool ? o->pool->mem + o->off : NULL);
}
// 零拷贝：CPU 侧写入对 GPU 立即可见，Flush/Invalidate 都是空操作（与内存池同口径）
// Zero-copy: CPU writes are immediately visible to the GPU; Flush/Invalidate are no-ops (same as memory pools)
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool*, const NVNbuffer *buf) { return (NVNmemoryPool *)((buf_t *)buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *buf) { return ((buf_t *)buf)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *buf) { return ((buf_t *)buf)->n; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *buf, const char *label) { (void)buf; (void)label; }
NVN(nvnBufferGetDebugID, uint64_t, const NVNbuffer *buf) { return (uint64_t)(uintptr_t)buf; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }
