#!/usr/bin/env python3
"""Supply all RTG message icons in the locally provided TrueType shared font.

Keeps the original text glyphs. Circled ABXY reuse existing Unicode outlines;
other controls are original vector drawings, not Nintendo's system-font assets.
Run --messages against the full RomFS/mesg tree to reject unknown PUA symbols.
Requires fontTools; no font is downloaded.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from rtg_message_symbols import private_symbols

# Source: author-maintained NintendoExt003 mapping; cross-checked against the
# actual full-game MSBT labels. E0AB/AC/AE describe face-key positions, not ABXY.
SYMBOLS={
    0xe0ab:'face-right',0xe0ac:'face-down',0xe0ae:'face-left',
    0xe0e0:'A',0xe0e1:'B',0xe0e2:'X',0xe0e3:'Y',
    0xe0e4:'L',0xe0e5:'R',0xe0e6:'ZL',0xe0e7:'ZR',0xe0e8:'SL',0xe0e9:'SR',
    0xe0eb:'up',0xe0ec:'down',0xe0ed:'left',0xe0ee:'right',
    0xe0ef:'plus',0xe0f0:'minus',0xe127:'horizontal-joycon',
}
ALIASES={0xe0e0:0x24b6,0xe0e1:0x24b7,0xe0e2:0x24cd,0xe0e3:0x24ce}


def contour(pen,points,hole=False):
    points=list(points)
    if hole:points.reverse()
    pen.moveTo(points[0])
    for point in points[1:]:pen.lineTo(point)
    pen.closePath()


def circle(pen,x,y,r,hole=False):
    contour(pen,[(round(x+r*math.cos(-2*math.pi*i/48)),round(y+r*math.sin(-2*math.pi*i/48))) for i in range(48)],hole)


def rounded(pen,x0,y0,x1,y1,r,hole=False):
    # Clockwise rounded rectangle, with inner contours reversed for holes.
    points=[]
    for x,y,start in [(x1-r,y1-r,90),(x1-r,y0+r,0),(x0+r,y0+r,-90),(x0+r,y1-r,-180)]:
        for i in range(9):
            a=math.radians(start-i*90/8);points.append((round(x+r*math.cos(a)),round(y+r*math.sin(a))))
    contour(pen,points,hole)


def ring(pen,x=1024,y=634,r=856,stroke=95):
    circle(pen,x,y,r);circle(pen,x,y,r-stroke,True)


def text(pen,font,label,x0,y0,x1,y1):
    glyphset=font.getGlyphSet();cmap=font.getBestCmap()
    names=[cmap[ord(c)] for c in label]
    bounds=[]
    for name in names:
        g=font['glyf'][name];g.recalcBounds(font['glyf']);bounds.append(g)
    width=sum(font['hmtx'][n][0] for n in names)-bounds[0].xMin-(font['hmtx'][names[-1]][0]-bounds[-1].xMax)
    low=min(g.yMin for g in bounds);high=max(g.yMax for g in bounds)
    scale=min((x1-x0)/width,(y1-y0)/(high-low))
    x=(x0+x1-width*scale)/2-bounds[0].xMin*scale;y=(y0+y1-(high-low)*scale)/2-low*scale
    for name in names:
        glyphset[name].draw(TransformPen(pen,(scale,0,0,scale,x,y)))
        x+=font['hmtx'][name][0]*scale


def draw(font,semantic):
    pen=TTGlyphPen(None)
    if semantic in ('L','R','ZL','ZR','SL','SR'):
        rounded(pen,128,-60,1920,1328,240);rounded(pen,228,40,1820,1228,140,True)
        text(pen,font,semantic,365,210,1683,1060)
    elif semantic in ('plus','minus'):
        ring(pen)
        contour(pen,[(520,544),(520,724),(1528,724),(1528,544)])
        if semantic=='plus':
            # Keep contours non-overlapping; touching edges form the plus.
            contour(pen,[(934,130),(934,544),(1114,544),(1114,130)])
            contour(pen,[(934,724),(934,1138),(1114,1138),(1114,724)])
    elif semantic in ('up','down','left','right'):
        ring(pen)
        points=[(-110,-490),(-110,140),(-400,140),(0,530),(400,140),(110,140),(110,-490)]
        turns={'up':0,'right':1,'down':2,'left':3}[semantic]
        for _ in range(turns):points=[(y,-x) for x,y in points]
        contour(pen,[(1024+x,634+y) for x,y in points])
    elif semantic.startswith('face-'):
        active=semantic[5:]
        for position,x,y in [('up',1024,1206),('right',1596,634),('down',1024,62),('left',452,634)]:
            circle(pen,x,y,270)
            if position!=active:circle(pen,x,y,185,True)
    elif semantic=='horizontal-joycon':
        # Distinct horizontal controller silhouette; do not turn this into A/B.
        rounded(pen,66,150,1982,1118,420);rounded(pen,160,244,1888,1024,326,True)
        ring(pen,520,634,205,75)
        for x,y in [(1450,834),(1650,634),(1450,434),(1250,634)]:circle(pen,x,y,74)
        rounded(pen,650,1118,900,1218,28);rounded(pen,1148,1118,1398,1218,28)
    else:raise ValueError(f'Unknown symbol {semantic}')
    return pen.glyph()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--messages',type=Path,required=True)
    a=p.parse_args();used,files,messages=private_symbols(a.messages)
    unknown=used-SYMBOLS.keys()
    if unknown:raise ValueError('Unmapped message symbols: '+', '.join(f'U+{c:04X}' for c in sorted(unknown)))
    f=TTFont(a.source,recalcTimestamp=False);original=f.getBestCmap().copy()
    if 'glyf' not in f:raise ValueError('A TrueType outline source is required')
    if f['head'].unitsPerEm!=2048:raise ValueError('This calibrated icon set requires a 2048-unit source font')
    order=list(f.getGlyphOrder());mapped={}
    for code,semantic in SYMBOLS.items():
        if code in ALIASES:
            source=ALIASES[code]
            if source not in original or f.getGlyphID(original[source])==0:raise ValueError(f'Base lacks U+{source:04X}')
            name=original[source]
        else:
            name=f'rtg_{code:04x}_{semantic.replace("-","_")}'
            if name in order:raise ValueError('Use the original source font, not a previously generated font')
            glyph=draw(f,semantic);f['glyf'][name]=glyph;order.append(name)
            glyph.recalcBounds(f['glyf']);f['hmtx'][name]=(2048,glyph.xMin)
        if code in original and original[code]!=name:raise ValueError(f'Would replace existing U+{code:04X}')
        mapped[code]=name
    f.setGlyphOrder(order)
    for table in f['cmap'].tables:
        if table.isUnicode() and table.format in (4,12):table.cmap.update(mapped)
    if 'DSIG' in f:del f['DSIG']
    a.output.parent.mkdir(parents=True,exist_ok=True);f.save(a.output)
    saved=TTFont(a.output);final=saved.getBestCmap()
    assert all(final.get(code)==name for code,name in original.items())
    # post format 3 stores no glyph names: generated names are reconstructed
    # from Unicode on reload, but glyph IDs must still route to the same data.
    assert all(saved.getGlyphID(final[code])==f.getGlyphID(name) for code,name in mapped.items())
    report={'source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),
        'output_sha256':hashlib.sha256(a.output.read_bytes()).hexdigest(),
        'controller_symbols':{f'U+{c:04X}':s for c,s in SYMBOLS.items()},
        'message_symbols':[f'U+{c:04X}' for c in sorted(used)],'message_files':files,'messages':messages,
        'original_characters_preserved':len(original),'mapping_source':'https://gist.github.com/offalynne/74445027027fc8dd0e789ba42f143470',
        'artwork':'Unicode circled ABXY plus original vector compatibility icons; E127 is a semantic controller substitute'}
    a.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
