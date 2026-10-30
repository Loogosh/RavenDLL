#!/usr/bin/env python3
"""Scan a live process for a 64-bit value (little-endian) and dump around hits.

Usage:
    python tools/memscan.py --pid 1234 --qword 0x000000000000000A --max 8 --window 0x400
    python tools/memscan.py --pid 1234 --qword 0x... --expect-float 4281.5
"""
import argparse
import ctypes
import struct
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__a1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__a2", wintypes.DWORD),
    ]


k32.OpenProcess.restype = wintypes.HANDLE
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.restype = wintypes.BOOL
k32.VirtualProtectEx.restype = wintypes.BOOL
k32.WriteProcessMemory.restype = wintypes.BOOL


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    if not ok:
        return b""
    return buf.raw[: got.value]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--qword", help="hex value, e.g. 0x000000000000000A")
    ap.add_argument("--f32", type=float, help="scan for a float32 value")
    ap.add_argument("--f64", type=float, help="scan for a float64 value")
    ap.add_argument("--f32seq", help="comma-separated float32 sequence, e.g. 4281,-2829.4,0")
    ap.add_argument("--read", help="ADDR,LEN hex dump instead of scanning, e.g. 0x2730D97BB00,0x400")
    ap.add_argument("--table", help="ADDR,COUNT,STRIDE dump {name ptr, fn ptr} entries")
    ap.add_argument("--write", help="ADDR,HEXBYTES write to process memory")
    ap.add_argument("--list", action="store_true", help="print only hit addresses")
    ap.add_argument("--hex", help="scan for arbitrary hex bytes, e.g. e09304000000803f")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--end", type=lambda s: int(s, 0), default=0x7FFFFFFF0000)
    ap.add_argument("--max", type=int, default=8)
    ap.add_argument("--window", type=lambda s: int(s, 0), default=0x400)
    ap.add_argument("--expect-float", type=float, default=None,
                    help="only keep hits whose window contains this float32")
    args = ap.parse_args()

    if args.write:
        a, hx = args.write.split(",")
        a = int(a, 0)
        data = bytes.fromhex(hx)
        h = k32.OpenProcess(0x0008 | 0x0020 | 0x0010 | 0x0400, False, args.pid)
        if not h:
            raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
        old = wintypes.DWORD(0)
        ok = k32.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), 0x04, ctypes.byref(old))
        if not ok:
            raise SystemExit(f"VirtualProtectEx failed: {ctypes.get_last_error()}")
        got = ctypes.c_size_t(0)
        ok = k32.WriteProcessMemory(h, ctypes.c_void_p(a), data, len(data), ctypes.byref(got))
        k32.VirtualProtectEx(h, ctypes.c_void_p(a), len(data), old.value, ctypes.byref(old))
        if not ok:
            raise SystemExit(f"WriteProcessMemory failed: {ctypes.get_last_error()}")
        print(f"wrote {got.value} bytes at 0x{a:X}")
        return

    if args.table:
        a, cnt, stride = (int(x, 0) for x in args.table.split(","))
        h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, args.pid)
        if not h:
            raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
        for i in range(cnt):
            ent = read(h, a + i * stride, 16)
            if len(ent) < 16:
                break
            namep, fn = struct.unpack("<QQ", ent)
            nm = read(h, namep, 64).split(b"\x00")[0].decode("ascii", "replace") if namep else ""
            print(f"[{i:3}] {nm:44} name=0x{namep:X} fn=0x{fn:X}")
        return

    if args.read:
        a, ln = (int(x, 0) for x in args.read.split(","))
        h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, args.pid)
        if not h:
            raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
        win = read(h, a, ln)
        for r in range(0, len(win), 16):
            chunk = win[r:r + 16]
            hexs = " ".join(f"{b:02x}" for b in chunk)
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  0x{a + r:012X}  {hexs:<48}  {asc}")
        return

    if args.hex:
        needle = bytes.fromhex(args.hex)
    elif args.f32seq:
        needle = b"".join(struct.pack("<f", float(x)) for x in args.f32seq.split(","))
    elif args.f32 is not None:
        needle = struct.pack("<f", args.f32)
    elif args.f64 is not None:
        needle = struct.pack("<d", args.f64)
    elif args.qword is not None:
        needle = struct.pack("<Q", int(args.qword, 0) & 0xFFFFFFFFFFFFFFFF)
    else:
        raise SystemExit("one of --qword/--f32/--f64 is required")
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, args.pid)
    if not h:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")

    addr = 0
    hits = []
    scanned = 0
    addr = args.start
    while addr < args.end and len(hits) < args.max:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            addr += 0x1000
            continue
        base, size = mbi.BaseAddress, mbi.RegionSize
        nxt = base + size
        if (mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) in
                (0x02, 0x04, 0x08, 0x20, 0x40, 0x80) and not (mbi.Protect & PAGE_GUARD)
                and size <= 1024 * 1024 * 1024):
            chunk = 4 * 1024 * 1024
            off = 0
            while off < size and len(hits) < args.max:
                take = min(chunk, size - off)
                data = read(h, base + off, take)
                if data:
                    scanned += len(data)
                    pos = 0
                    while True:
                        i = data.find(needle, pos)
                        if i < 0:
                            break
                        va = base + off + i
                        lo = max(0, va - args.window // 2)
                        win = read(h, lo, args.window)
                        if args.expect_float is not None:
                            f = struct.pack("<f", args.expect_float)
                            if f not in win:
                                pos = i + 1
                                continue
                        hits.append((va, lo, win))
                        pos = i + 1
                        if len(hits) >= args.max:
                            break
                off += take
        addr = nxt if nxt > addr else addr + 0x1000

    print(f"scanned={scanned} bytes, hits={len(hits)}")
    if args.list:
        for va, lo, win in hits:
            print(f"0x{va:X}")
        return
    for va, lo, win in hits:
        print(f"\\nHIT 0x{va:X} (window 0x{lo:X})")
        for r in range(0, len(win), 16):
            chunk = win[r:r + 16]
            hexs = " ".join(f"{b:02x}" for b in chunk)
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  0x{lo + r:012X}  {hexs:<48}  {asc}")


if __name__ == "__main__":
    main()






