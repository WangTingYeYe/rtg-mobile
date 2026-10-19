#!/usr/bin/env python3
"""Build and validate RTG's Vulkan cache from retained NVN shader dumps.

Uses the existing Maxwell translator, SPIRV-Tools, and SPIRV-Cross. The renderer
requires full control-block indexing, Vulkan descriptor types, vertex inputs,
and separate descriptor sets for vertex/fragment stages. Metal caches cannot be
substituted. Unsupported resource layouts fail explicitly.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


def digest(data):
    return hashlib.sha256(data).hexdigest()


def code_hash(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def instructions(data):
    words = list(struct.unpack('<' + 'I' * (len(data) // 4), data))
    if words[0] != 0x07230203:
        raise ValueError('Invalid SPIR-V magic')
    out, offset = [], 5
    while offset < len(words):
        count = words[offset] >> 16
        if not count or offset + count > len(words):
            raise ValueError('Invalid SPIR-V instruction length')
        out.append(words[offset:offset + count])
        offset += count
    return words[:5], out


def encode(header, records):
    words = header + [word for record in records for word in record]
    return struct.pack('<' + 'I' * len(words), *words)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, default=root / 'run/shaderdump')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--shader-tools', type=Path, required=True)
    parser.add_argument('--spirv-cross', default=shutil.which('spirv-cross'))
    parser.add_argument('--translator', type=Path, default=root / 'rtg/gfx/tools/dmsh_xlate/bin/Release/net10.0/dmsh_xlate')
    parser.add_argument('--dotnet-root', type=Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.spirv_cross:
        parser.error('spirv-cross is required')
    env = os.environ.copy()
    if args.dotnet_root:
        env['DOTNET_ROOT'] = str(args.dotnet_root.resolve())

    def run(*command):
        result = subprocess.run([str(x) for x in command], env=env,
                                capture_output=True, text=True, check=True)
        return result.stdout

    jobs, sources = [], {}
    for control in sorted(args.dump.glob('*_ctl.bin')):
        match = re.fullmatch(r'.*_st([01])_ctl', control.stem)
        if not match:
            raise ValueError(f'Unsupported shader stage: {control}')
        stage = int(match[1])
        ctl = control.read_bytes()
        if len(ctl) != 0x880:
            raise ValueError(f'Incomplete control block: {control}')
        data = control.with_name(control.name.replace('_ctl', '_data')).read_bytes()
        # RTG has distinct programs with identical NVN control blocks. Include
        # the code/constants rather than silently selecting the first program.
        key = digest(ctl + data)[:16]
        if key in sources:
            if sources[key]['stage'] != stage or sources[key]['data_sha256'] != digest(data):
                raise ValueError(f'Conflicting shader dump: {key}')
            continue
        c1start, c1end = struct.unpack_from('<2I', ctl, 0x700)
        if not 0x80 <= c1start <= c1end <= len(data):
            raise ValueError(f'Incomplete code/constants: {control}')
        code = args.work / (key + '.code')
        code.write_bytes(data[0x30:])
        sources[key] = dict(stage=stage, ctl=ctl, control=str(control),
                            data_sha256=digest(data), c1=data[c1start:c1end])
        jobs.append(dict(key=key, stage='vs' if stage == 0 else 'ps',
                         code=str(code.resolve()), tex={}, img={}, attr={}, fout={},
                         c1=data[c1start:c1end].hex(), debug=False, reduced=False,
                         vector_indexing_workaround=True))
        # Captured RTG sprite draws use RGB32F/RG32F/R32UI at locations 0/1/2.
        # The instance index must remain integer through Vulkan vertex fetch.
        # The original Maxwell code uses its raw bits to index vp_c3.
        if stage == 0 and key == 'adb3f1953400f0dc':
            jobs[-1]['attr'] = {'2': 'uint'}
    if not jobs:
        raise ValueError('No shader dumps found')
    jobfile = args.work / 'jobs.json'
    jobfile.write_text(json.dumps(jobs, indent=2))
    translated = args.work / 'translated'
    print(run(args.translator.resolve(), jobfile.resolve(), translated.resolve()).strip())
    index, provenance = bytearray(struct.pack('<3I', 0x494b5644, 2, len(jobs))), []
    for job in jobs:
        key = job['key']
        source = sources[key]
        stage, setno = source['stage'], source['stage']
        module = args.output / (key + '.spv')
        run(args.shader_tools / 'spirv-opt', '-O', translated / (key + '.spv'), '-o', module)
        header, records = instructions(module.read_bytes())
        for record in records:
            op = record[0] & 0xffff
            if op == 15 and record[1] != (0 if stage == 0 else 4):
                raise ValueError(f'Shader execution model mismatch: {key}')
            if op == 71 and record[2] == 34:
                if record[3] != 0:
                    raise ValueError('Translator returned an unexpected descriptor set')
                record[3] = setno
        module.write_bytes(encode(header, records))
        run(args.shader_tools / 'spirv-val', '--target-env', 'vulkan1.1', module)
        # This translator declares optional capabilities even when unused. The
        # bundled older optimizer lacks --trim-capabilities. Remove a declaration
        # only when the Vulkan validator proves the remaining module valid.
        for record in list(records):
            if record[0] & 0xffff not in (10, 17):
                continue
            if record[0] & 0xffff == 17 and record[1] == 1:
                continue  # Keep Shader so unused optional capabilities cannot imply it.
            candidate = [r for r in records if r is not record]
            module.write_bytes(encode(header, candidate))
            valid = subprocess.run([str(args.shader_tools / 'spirv-val'), '--target-env',
                                    'vulkan1.1', str(module)], capture_output=True).returncode == 0
            if valid:
                records = candidate
        module.write_bytes(encode(header, records))
        run(args.shader_tools / 'spirv-val', '--target-env', 'vulkan1.1', module)
        reflection = json.loads(run(args.spirv_cross, module, '--reflect'))
        resources = json.loads((translated / (key + '.res.json')).read_text())
        if resources['sbufs'] or resources['images'] or reflection.get('ssbos') or reflection.get('images'):
            raise ValueError(f'Unsupported storage resource in {key}')
        cbufs = {r['binding']: r['slot'] for r in resources['cbufs']}
        textures = {r['binding']: r for r in resources['textures']}
        bindings = []
        for uniform in reflection.get('ubos', []):
            binding = uniform['binding']
            if uniform['name'] == 'support_buffer':
                kind, slot = 1, 0
            else:
                bank = cbufs[binding]
                if bank == 1:
                    if not source['c1']:
                        raise ValueError(f'Missing shader constants: {key}')
                    kind, slot = 7, 0
                elif 3 <= bank <= 16:
                    kind, slot = 2, bank - 3
                else:
                    raise ValueError(f'Unsupported driver constant bank {bank}: {key}')
            bindings.append((setno, binding, 6, kind, slot, -1, 1, 0))
        for texture in reflection.get('textures', []):
            t = textures[texture['binding']]
            if (t['separate'] or t['array'] != 1 or t['type'] != 3
                    or t['cbuf_slot'] not in (-1, 2) or not 8 <= t['handle'] < 264
                    or t['handle'] % 2):
                raise ValueError(f'Unsupported texture handle: {t}')
            # RTG SDK 21.4 uses c2 byte 0x20 (word 8), two words per slot.
            bindings.append((setno, t['binding'], 1, 4, (t['handle'] - 8) // 2, -1, 1, 0))
        attrs = []
        if stage == 0:
            for entry in reflection.get('inputs', []):
                typ = entry['type']
                kind = 1 if typ.startswith(('uvec', 'uint')) else 2 if typ.startswith(('ivec', 'int')) else 0
                attrs.append((entry['location'], kind))
        caps = [r[1] for r in records if r[0] & 0xffff == 17]
        outputs = sum(1 << e['location'] for e in reflection.get('outputs', [])) if stage == 1 else 0
        c1 = source['c1']
        meta = struct.pack('<12I', 0x4d4b5644, 1, stage, len(bindings), len(attrs),
                           len(c1), len(caps), 0, 0, 0, outputs, 0)
        meta += b''.join(struct.pack('<4I2i2I', *b) for b in sorted(bindings))
        meta += b''.join(struct.pack('<2I', *a) for a in sorted(attrs))
        meta += b''.join(struct.pack('<I', c) for c in caps) + c1
        (args.output / (key + '.vkmeta')).write_bytes(meta)
        data = Path(source['control']).with_name(Path(source['control']).name.replace('_ctl', '_data')).read_bytes()
        index.extend(key.encode() + struct.pack('<I', stage) + source['ctl'] + struct.pack('<Q', code_hash(data)))
        provenance.append(dict(key=key, stage=stage, control=source['control'],
                               data_sha256=source['data_sha256'], spv_sha256=digest(module.read_bytes()),
                               metadata_sha256=digest(meta), capabilities=caps))
    (args.output / 'shader_index.bin').write_bytes(index)
    manifest = {'shaders': provenance, 'files': [dict(name=p.name, bytes=p.stat().st_size,
                sha256=digest(p.read_bytes())) for p in sorted(args.output.iterdir())
                if p.suffix in ('.spv', '.vkmeta', '.bin')]}
    (args.output / 'shader-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Validated {len(jobs)} shaders; cache: {args.output}')


if __name__ == '__main__':
    main()
