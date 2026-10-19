#include "../nvn/nvn_vk_shader_identity.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    NvnShaderCodeSnapshot slot = {0};
    uint8_t a[2048], b[2048];
    memset(a, 0x34, sizeof a);memcpy(b, a, sizeof a);
    uint64_t original = nvn_shader_code_hash(a, sizeof a);
    assert(nvn_shader_code_hash_cached(&slot, a, sizeof a) == original);
    assert(nvn_shader_code_hash_cached(&slot, b, sizeof b) == original);
    /* Same guest address/control, different code: must not alias the old shader. */
    a[1793] ^= 1;
    uint64_t changed = nvn_shader_code_hash(a, sizeof a);
    assert(changed != original);
    assert(nvn_shader_code_hash_cached(&slot, a, sizeof a) == changed);
    assert(nvn_shader_code_hash_cached(&slot, b, sizeof b) == original);
    assert(nvn_shader_code_hash_cached(&slot, a, 1024) == nvn_shader_code_hash(a, 1024));
    assert(nvn_shader_code_hash_cached(&slot, a, sizeof a) == changed);
    free(slot.bytes);
    puts("PASS exact code identity: repeat, equal bytes at new address, mutation, reuse, extent change");
}
