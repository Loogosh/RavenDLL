#!/usr/bin/env python3
"""Disassemble a small range of the dumped 1.14 image by RVA.

Usage: python tools/disasm_rva.py 0x199aac0 0x120
"""
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE = Path(r"F:\mm_work\WowClassic_dumpimage.exe")


def sections(b, e, nsec, optsz, opt):
    sec = opt + optsz
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = b[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vs, va, rs, ptr = struct.unpack_from("<IIII", b, o + 8)
        out.append((name, va, vs, ptr, rs))
    return out


def main(argv):
    rva = int(argv[0], 0)
    size = int(argv[1], 0) if len(argv) > 1 else 0x80
    b = PE.read_bytes()
    e = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, e + 6)[0]
    optsz = struct.unpack_from("<H", b, e + 20)[0]
    opt = e + 24
    imgbase = struct.unpack_from("<Q", b, opt + 24)[0]
    secs = sections(b, e, nsec, optsz, opt)

    def r2o(a):
        for name, va, vs, ptr, rs in secs:
            if va <= a < va + max(vs, rs):
                return ptr + (a - va)
        return None

    off = r2o(rva)
    if off is None:
        raise SystemExit(f"RVA 0x{rva:X} not mapped")
    code = b[off:off + size]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    print(f"RVA 0x{rva:X} VA 0x{imgbase + rva:X}")
    for insn in md.disasm(code, imgbase + rva):
        print(f"  0x{insn.address - imgbase:X}: {insn.bytes.hex(' '):<24} {insn.mnemonic:<8} {insn.op_str}")


if __name__ == "__main__":
    main(sys.argv[1:])
