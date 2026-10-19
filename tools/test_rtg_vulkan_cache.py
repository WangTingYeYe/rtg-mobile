#!/usr/bin/env python3
"""Exercise the renderer's actual cache lookup with RTG's colliding controls.

Compiles the production shader_entry/control_hash functions against a host file
loader. Covers all retained shader variants, a changed program, and v1 support.
This verifies program selection, not GPU rendering.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=Path('build/rtg-shaders-v2'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'rtg/gfx/nvn/nvn_vk.c').read_text()
    hashing = source[source.index('static uint64_t control_hash('):source.index('static BindingPlan *binding_plan_compile(')]
    lookup = source[source.index('static const uint8_t *shader_entry('):source.index('static VShader *shader_get(')]
    fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_vk_shader_identity.h"
typedef struct { const uint8_t *entry; uint64_t hash; } ShaderEntry;
static struct { uint8_t *shader_index; size_t index_bytes,shader_entry_capacity; ShaderEntry *shader_entries; } G;
static char index_path[4096];
static void die(const char *what,long long value) { fprintf(stderr,"%s: %lld\n",what,value);abort(); }
static uint8_t *read_file(const char *name,size_t *size) {
    (void)name;FILE *f=fopen(index_path,"rb");if(!f)abort();fseek(f,0,SEEK_END);
    *size=(size_t)ftell(f);rewind(f);uint8_t *p=malloc(*size);if(fread(p,1,*size,f)!=*size)abort();fclose(f);return p;
}
void select_index(const char *path) { free(G.shader_index);free(G.shader_entries);memset(&G,0,sizeof G);snprintf(index_path,sizeof index_path,"%s",path); }
'''
    fixture += hashing + lookup + r'''
const char *find_program(const void *ctl,uint32_t stage,const void *code,size_t size) {
    static char key[17];const uint8_t *entry=shader_entry(ctl,stage,nvn_shader_code_hash(code,size));
    if(!entry)return NULL;memcpy(key,entry,16);key[16]=0;return key;
}
'''
    manifest = json.loads((args.cache / 'shader-manifest.json').read_text())
    with tempfile.TemporaryDirectory(prefix='rtg-cache-test-') as temporary:
        directory = Path(temporary)
        (directory / 'lookup.c').write_text(fixture)
        library = directory / 'lookup.dylib'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I', str(root / 'rtg/gfx/nvn'), str(directory / 'lookup.c'), '-o', str(library)], check=True)
        native = ctypes.CDLL(str(library))
        native.select_index.argtypes = [ctypes.c_char_p]
        native.find_program.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
        native.find_program.restype = ctypes.c_char_p
        native.select_index(str((args.cache / 'shader_index.bin').resolve()).encode())
        controls = {}
        for entry in manifest['shaders']:
            path = Path(entry['control'])
            ctl = path.read_bytes()
            code = path.with_name(path.name.replace('_ctl', '_data')).read_bytes()
            assert hashlib.sha256(ctl + code).hexdigest()[:16] == entry['key']
            actual = native.find_program(ctl, entry['stage'], code, len(code))
            assert actual == entry['key'].encode(), (entry['key'], actual)
            controls.setdefault((entry['stage'], ctl), []).append((entry['key'], code))
        collisions = [variants for variants in controls.values() if len(variants) > 1]
        assert collisions, 'Fixture must contain the real same-control/different-code bug'
        (stage, ctl), variants = next((k, v) for k, v in controls.items() if len(v) > 1)
        changed = bytearray(variants[0][1]); changed[-1] ^= 0x80
        assert native.find_program(ctl, stage, bytes(changed), len(changed)) is None
        legacy = directory / 'v1.bin'
        legacy.write_bytes(struct.pack('<3I', 0x494b5644, 1, 1) + variants[0][0].encode() + struct.pack('<I', stage) + ctl)
        native.select_index(str(legacy).encode())
        assert native.find_program(ctl, stage, variants[0][1], len(variants[0][1])) == variants[0][0].encode()
        # This is the old ambiguity: control-only v1 also selects the first key
        # for the other code. The v2 checks above must select each distinct key.
        assert native.find_program(ctl, stage, variants[1][1], len(variants[1][1])) == variants[0][0].encode()
    print(f"PASS: {len(manifest['shaders'])} code variants, {len(collisions)} ambiguous control groups, unknown-code rejection, v1 compatibility")


if __name__ == '__main__':
    main()
