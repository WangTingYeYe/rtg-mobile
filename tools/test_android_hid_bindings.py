#!/usr/bin/env python3
"""Check RTG's required controller overrides in the actual Android ELF library.

These SDK calls previously survived in the common tail of hle_hid.c, outside
its Apple-only branch. Losing that tail still links, but calls Horizon IPC at
startup (SetNpadJoyHoldType) or when sampling input / opening controller support.
No game binaries or device are required.
"""
import argparse
from pathlib import Path
import struct

REQUIRED = (
    '_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE',
    '_ZN2nn3hid14DisconnectNpadERKj',
    '_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj',
    '_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj',
    '_ZN2nn3hid20ControllerSupportArg10SetDefaultEv',
    '_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE',
    '_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE',
    '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj',
    '_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj',
)


def registered_names(data):
    if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('Expected little-endian ELF64 AArch64 library')
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count, strings = struct.unpack_from('<HHH', data, 58)
    if stride != 64 or not count or strings >= count:
        raise ValueError('Unsupported ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride) for i in range(count)]
    string_section = sections[strings]
    names = data[string_section[4]:string_section[4] + string_section[5]]
    named = {names[s[0]:].split(b'\0', 1)[0].decode(): s for s in sections}
    force = named['dm_force']
    if not force[5] or force[5] % 24:
        raise ValueError('Invalid HLE registration section')
    relocations = {}
    for section in sections:
        if section[1] == 4:  # SHT_RELA
            if section[9] != 24:
                raise ValueError('Unsupported RELA entry size')
            for at in range(section[4], section[4] + section[5], 24):
                address, info, addend = struct.unpack_from('<QQq', data, at)
                if info == 1027:  # R_AARCH64_RELATIVE; local name/function pointers.
                    relocations[address] = addend

    def file_offset(address):
        for s in sections:
            if s[1] != 8 and s[3] <= address < s[3] + s[5]:
                return s[4] + address - s[3]
        raise ValueError('Registration points outside file-backed sections')

    result = set()
    for at in range(0, force[5], 24):
        name = relocations[force[3] + at]
        function = relocations[force[3] + at + 8]
        if not any(s[2] & 4 and s[3] <= function < s[3] + s[5] for s in sections):
            raise ValueError('HLE registration does not point to executable code')
        start = file_offset(name)
        result.add(data[start:data.index(b'\0', start)].decode())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    args = parser.parse_args()
    names = registered_names(args.native.read_bytes())
    missing = sorted(set(REQUIRED) - names)
    for name in missing:
        print('MISSING ' + name)
    print(f'{"FAIL" if missing else "PASS"}: {len(REQUIRED)} required controller bindings, {len(missing)} missing; {len(names)} registered overrides')
    return bool(missing)


if __name__ == '__main__':
    raise SystemExit(main())
