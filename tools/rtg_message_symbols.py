"""Read visible text from MSBT, excluding binary tags, for font coverage checks."""
import struct
from pathlib import Path


def texts(path):
    data=Path(path).read_bytes()
    if data[:8]!=b'MsgStdBn':raise ValueError(f'{path}: not MSBT')
    endian={b'\xff\xfe':'<',b'\xfe\xff':'>'}.get(data[8:10])
    if endian is None:raise ValueError(f'{path}: invalid byte order')
    enc=data[12]
    if enc not in (0,1,2):raise ValueError(f'{path}: unsupported encoding {enc}')
    unit=(1,2,4)[enc]
    encoding='utf-8' if enc==0 else ('utf-16-' if enc==1 else 'utf-32-')+('le' if endian=='<' else 'be')
    offset=32;section=None
    while offset+16<=len(data):
        size=struct.unpack_from(endian+'I',data,offset+4)[0]
        if offset+16+size>len(data):raise ValueError(f'{path}: truncated section')
        if data[offset:offset+4]==b'TXT2':section=data[offset+16:offset+16+size];break
        offset=(offset+16+size+15)&~15
    if section is None:raise ValueError(f'{path}: missing TXT2')
    count=struct.unpack_from(endian+'I',section)[0]
    offsets=struct.unpack_from(endian+'I'*count,section,4)
    for index,start in enumerate(offsets):
        end=offsets[index+1] if index+1<count else len(section)
        if not 4+4*count<=start<=end<=len(section):raise ValueError(f'{path}: invalid text offset')
        raw=section[start:end];i=0;clean=bytearray()
        while i+unit<=len(raw):
            c=int.from_bytes(raw[i:i+unit],'little' if endian=='<' else 'big')
            if c==0:break
            if c==14:
                if i+unit+6>len(raw):raise ValueError(f'{path}: truncated tag')
                n=struct.unpack_from(endian+'H',raw,i+unit+4)[0];i+=unit+6+n
                if i>len(raw):raise ValueError(f'{path}: truncated tag payload')
            elif c==15:i+=unit+4
            else:clean.extend(raw[i:i+unit]);i+=unit
        yield clean.decode(encoding)


def private_symbols(directory):
    files=sorted(Path(directory).rglob('*.msbt'))
    if not files:raise ValueError(f'{directory}: no MSBT messages')
    codes=set();messages=0
    for path in files:
        for text in texts(path):
            messages+=1
            codes.update(ord(c) for c in text if 0xe000<=ord(c)<=0xf8ff or 0xf0000<=ord(c)<=0xffffd or 0x100000<=ord(c)<=0x10fffd)
    return codes,len(files),messages
