#!/usr/bin/env python3
"""Compare cache input types with a captured real RTG Vulkan draw trace."""
import argparse
from pathlib import Path
import re
import struct


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    args=parser.parse_args()
    # Observed NVN formats: RGB32F, RG32F, and unsigned R32 instance indices.
    kinds={0x22:0,0x16:0,0x0b:1}
    draws={}; checked=set(); errors=[]
    for line in args.trace.read_text(errors='replace').splitlines():
        match=re.search(r'DRAW_TRACE draw=(\d+) vs=([0-9a-f]{16})',line)
        if match:draws[int(match[1])]=match[2]
        match=re.search(r'VERTEX_INPUT draw=(\d+) location=(\d+).*?format=(\d+)',line)
        if not match:continue
        draw,location,fmt=map(int,match.groups());key=draws[draw]
        if (key,location,fmt) in checked:continue
        checked.add((key,location,fmt))
        metadata=(args.cache/(key+'.vkmeta')).read_bytes()
        header=struct.unpack_from('<12I',metadata)
        attrs=dict(struct.unpack_from('<2I',metadata,48+32*header[3]+8*i) for i in range(header[4]))
        if attrs[location]!=kinds[fmt]:errors.append(f'{key} location {location}: NVN format {fmt:#x} requires kind {kinds[fmt]}, cache declares {attrs[location]}')
    if not checked:raise ValueError('No captured draw inputs')
    print('\n'.join(errors) if errors else f'PASS: {len(checked)} captured vertex inputs match numeric types')
    return bool(errors)


if __name__=='__main__':raise SystemExit(main())
