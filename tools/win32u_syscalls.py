#!/usr/bin/env python3
"""
Map win32u.dll (or ntdll.dll) export names to their system-service numbers by
reading each export's syscall stub straight out of the binary.

A Windows x64 syscall stub is
    4C 8B D1              mov r10, rcx
    B8 <imm32>            mov eax, ServiceNumber
    F6 04 25 .. ..        test byte ptr [7FFE0308h], 1   (optional)
    0F 05                 syscall
so the service number is the imm32 of the `mov eax` right after `mov r10, rcx`.

Usage:
    tools/win32u_syscalls.py win/win32u.dll            # list name -> number
    tools/win32u_syscalls.py win/win32u.dll 0x1403     # look one number up
"""
import struct
import sys


def read_pe(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0', 'not a PE image'
    coff = pe + 4
    nsections, = struct.unpack_from('<H', data, coff + 2)
    opt_size, = struct.unpack_from('<H', data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from('<H', data, opt)
    assert magic == 0x20B, 'not PE32+'
    dir_base = opt + 112
    export_rva, export_size = struct.unpack_from('<II', data, dir_base)
    sections = []
    sec = opt + opt_size
    for i in range(nsections):
        off = sec + i * 40
        vaddr, = struct.unpack_from('<I', data, off + 12)
        vsize, = struct.unpack_from('<I', data, off + 8)
        raw, = struct.unpack_from('<I', data, off + 20)
        rawsize, = struct.unpack_from('<I', data, off + 16)
        sections.append((vaddr, max(vsize, rawsize), raw))
    return data, sections, export_rva


def rva_to_off(sections, rva):
    for vaddr, vsize, raw in sections:
        if vaddr <= rva < vaddr + vsize:
            return raw + (rva - vaddr)
    return None


def exports(data, sections, export_rva):
    base = rva_to_off(sections, export_rva)
    nfunc, nname = struct.unpack_from('<II', data, base + 20)
    func_rva, name_rva, ord_rva = struct.unpack_from('<III', data, base + 28)
    funcs = rva_to_off(sections, func_rva)
    names = rva_to_off(sections, name_rva)
    ords_ = rva_to_off(sections, ord_rva)
    for i in range(nname):
        name_ptr, = struct.unpack_from('<I', data, names + i * 4)
        off = rva_to_off(sections, name_ptr)
        end = data.index(b'\0', off)
        name = data[off:end].decode('ascii', 'replace')
        index, = struct.unpack_from('<H', data, ords_ + i * 2)
        fn_rva, = struct.unpack_from('<I', data, funcs + index * 4)
        yield name, fn_rva


def service_number(data, sections, rva):
    off = rva_to_off(sections, rva)
    if off is None:
        return None
    code = data[off:off + 24]
    if code[:3] != b'\x4c\x8b\xd1' or code[3] != 0xB8:
        return None
    return struct.unpack_from('<I', code, 4)[0]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    data, sections, export_rva = read_pe(sys.argv[1])
    table = {}
    for name, rva in exports(data, sections, export_rva):
        num = service_number(data, sections, rva)
        if num is not None:
            table[num] = name
    if len(sys.argv) > 2:
        for arg in sys.argv[2:]:
            num = int(arg, 0)
            print('0x%04X  %s' % (num, table.get(num, '<not a service>')))
        return 0
    for num in sorted(table):
        print('0x%04X  %s' % (num, table[num]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
