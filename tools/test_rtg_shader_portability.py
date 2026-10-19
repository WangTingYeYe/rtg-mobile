#!/usr/bin/env python3
"""Audit the Android SPIR-V cache for two reproduced Adreno 630 hazards."""
import argparse
from pathlib import Path
from build_rtg_vulkan import instructions


def hazards(data):
    _, records = instructions(data)
    types, result_types, constants = {}, {}, {}
    for r in records:
        op = r[0] & 65535
        if 19 <= op <= 39:
            types[r[1]] = (op, r[2:])
        elif op == 43:
            constants[r[2]] = r[3]
        elif op in (59, 65, 66):
            result_types[r[2]] = r[1]
    errors, clip_counts = [], {}
    for r in records:
        op = r[0] & 65535
        if op == 72 and len(r) == 5 and r[3] == 11 and r[4] in (3, 4):
            struct, member = r[1:3]
            _, members = types[struct]
            array_op, array = types[members[member]]
            if array_op == 28:
                clip_counts[struct] = clip_counts.get(struct, 0) + constants[array[1]]
        if op not in (65, 66):
            continue
        pointer_op, pointer = types[result_types[r[3]]]
        if pointer_op != 32 or pointer[0] != 2:  # Uniform storage only.
            continue
        current = pointer[1]
        for index in r[4:]:
            kind, fields = types[current]
            if kind == 30:
                current = fields[constants[index]]
            elif kind in (28, 29):
                current = fields[0]
            elif kind == 23:
                if index not in constants:
                    errors.append(f'dynamic uniform vector component in access %{r[2]}')
                current = fields[0]
            else:
                raise ValueError(f'Unexpected access-chain type {kind}')
    errors.extend(f'clip/cull interface %{s} declares {n} distances (portable limit 8)'
                  for s, n in clip_counts.items() if n > 8)
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=Path, required=True)
    a = p.parse_args()
    files = sorted(a.cache.glob('*.spv'))
    if not files:
        raise SystemExit('No shader modules')
    failures = {f.name: hazards(f.read_bytes()) for f in files}
    failures = {name: errors for name, errors in failures.items() if errors}
    for name, errors in failures.items():
        print(f'{name}: {len(errors)} hazards; {errors[0]}')
    print(f'{"FAIL" if failures else "PASS"}: {len(files)} shaders, {len(failures)} affected modules')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
