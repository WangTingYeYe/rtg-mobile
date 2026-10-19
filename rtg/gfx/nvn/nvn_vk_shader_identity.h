#ifndef NVN_VK_SHADER_IDENTITY_H
#define NVN_VK_SHADER_IDENTITY_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cache v2 disambiguates RTG programs sharing the same control block. Code
 * length is the validated control+0x704 extent, including embedded constants. */
static inline uint64_t nvn_shader_code_hash(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Exact-byte validation remains necessary: different RTG shaders can share a
 * control block, and guest addresses can be reused. Optimized libc memcmp is
 * much cheaper than the serial FNV multiply chain on every draw. The caller
 * owns a bounded set of slots; allocation failure falls back to full hashing. */
typedef struct {
    uint8_t *bytes;
    size_t size;
    uint64_t hash;
} NvnShaderCodeSnapshot;

static inline uint64_t nvn_shader_code_hash_cached(NvnShaderCodeSnapshot *slot,
                                                  const void *data, size_t size) {
    if (slot->bytes && slot->size == size && !memcmp(slot->bytes, data, size))
        return slot->hash;
    uint64_t hash = nvn_shader_code_hash(data, size);
    if (slot->size != size || !slot->bytes) {
        uint8_t *bytes = realloc(slot->bytes, size);
        if (!bytes) return hash;
        slot->bytes = bytes;
        slot->size = size;
    }
    memcpy(slot->bytes, data, size);
    slot->hash = hash;
    return hash;
}
#endif
