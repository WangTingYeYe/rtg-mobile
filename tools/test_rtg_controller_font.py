#!/usr/bin/env python3
"""Check every PUA symbol used by the game's actual messages in the shipped TTF.

Use --messages with the complete RomFS/mesg tree, including all languages.
This checks real font outlines and cmap routing; device screenshots also verify
that the guest font service loaded the new asset and rasterized the prompts.
"""
import argparse
from pathlib import Path
from fontTools.ttLib import TTFont
from rtg_message_symbols import private_symbols


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--font',type=Path,required=True)
    p.add_argument('--messages',type=Path,required=True)
    p.add_argument('--source',type=Path,help='Verify all original cmap entries and metrics remain unchanged')
    a=p.parse_args();font=TTFont(a.font,checkChecksums=2);cmap=font.getBestCmap()
    codes,files,messages=private_symbols(a.messages)
    print(f'MSBT files={files} messages={messages} private_symbols={len(codes)}')
    failed=False
    for code in sorted(codes):
        name=cmap.get(code);glyph=font['glyf'][name] if name else None
        valid=bool(name and font.getGlyphID(name)!=0 and name!=cmap.get(0x25a1) and glyph.numberOfContours!=0)
        if valid:
            glyph.recalcBounds(font['glyf'])
            valid=glyph.xMax>glyph.xMin and glyph.yMax>glyph.yMin and font['hmtx'][name][0]>0
        tables=[t for t in font['cmap'].tables if t.isUnicode() and t.format in (4,12)]
        valid &= bool(tables) and all(t.cmap.get(code)==name for t in tables)
        print(f'{"PASS" if valid else "FAIL"}: U+{code:04X} visible outline={name}')
        failed|=not valid
    for code in [ord('准'),ord('跳'),ord('环'),0x25a1]:
        valid=cmap.get(code) is not None and font.getGlyphID(cmap[code])!=0
        print(f'{"PASS" if valid else "FAIL"}: existing U+{code:04X}');failed|=not valid
    if a.source:
        original=TTFont(a.source);oldmap=original.getBestCmap()
        valid=all(cmap.get(c)==g and font['hmtx'][g]==original['hmtx'][g] and
                  font['glyf'][g].compile(font['glyf'])==original['glyf'][g].compile(original['glyf']) for c,g in oldmap.items())
        print(f'{"PASS" if valid else "FAIL"}: preserved {len(oldmap)} original characters, outlines and metrics');failed|=not valid
    return int(failed)


if __name__=='__main__':raise SystemExit(main())
