#!/usr/bin/env python
"""Convert a gettext .po file into the custom YMO resource format.

YMO layout (little endian):
    uint16            entry count
    { uint32 fnv1a32 hash, uint16 offset } * count
    payload blob: UTF-16LE strings, each NUL terminated (2 bytes)

Dependency: polib (pure python, PyPI).
The original version used translate-toolkit, which had to be installed from a
git URL and pulled in lxml - needlessly heavy and unreliable for CI.
"""
import sys

import polib

FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193


def fnv1a_32(data, hval=FNV1_32_INIT):
    for byte in data:
        hval ^= byte
        hval = (hval * FNV_32_PRIME) & 0xffffffff
    return hval


def po2ymo(infile, outfile, includefuzzy=False, encoding='utf-16le'):
    inputstore = polib.pofile(infile.name)

    units = {}
    for unit in inputstore:
        source = unit.msgid
        context = unit.msgctxt
        target = unit.msgstr

        if not target:
            continue
        if unit.obsolete or unit.fuzzy:
            if not (includefuzzy and unit.fuzzy):
                continue

        if context:
            source = context + '\004' + source

        hash = fnv1a_32(source.encode(encoding))
        units[hash] = target.encode(encoding) + bytes(2)

    byteorder = 'little'
    outfile.write(len(units).to_bytes(2, byteorder))  # len

    offset = 2 + len(units) * (4 + 2)
    for hash, data in units.items():
        outfile.write(hash.to_bytes(4, byteorder))
        outfile.write(offset.to_bytes(2, byteorder))
        offset += len(data)

    for data in units.values():
        outfile.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("usage: po2ymo.py <infile> <outfile>")
        sys.exit(1)
    infile = open(sys.argv[1], 'rb')
    outfile = open(sys.argv[2], 'wb')
    po2ymo(infile, outfile)
