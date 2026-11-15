#!/usr/bin/env python3
"""Bit-exact check of FastTerrain (src/terrain_fast.cpp) against the game.

Runs Mcm2.exe's own terrain code and harness.dll (build.sh: the rewrites with
cdecl entry points) side by side in an x86 emulator (unicorn) on random
terrain trees, frame after frame with a moving camera, and compares every
byte they write:

  Detail 0x482550   per node: return value + node data + terrain counters
  Eval   0x4836e0   whole tree: all node, data and terrain bytes
  Draw   0x4818b0   the sequence of draw calls (0x481c00 / 0x481f20)

The game's start-up tables (square root, detail bands, vertex dependencies)
are built by running the game's own initialisers. The game's state reset
(0x483650) gives realistic vertex flags to start from. The FPU runs at 53-bit
precision as in the game.

usage: terrain_harness.py <Mcm2.exe> [harness.dll] [--trees N] [--frames N] [--seed S]
"""
import argparse
import random
import struct
import sys

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP, UC_X86_REG_EIP,
                               UC_X86_REG_FPCW, UC_X86_REG_CR0, UC_X86_REG_CR4, UC_X86_REG_FPTAG)

EXE_BASE = 0x400000
DLL_BASE = 0x20000000
HEAP = 0x30000000
HEAP_SIZE = 0x02000000
STACK = 0x70000000
STACK_SIZE = 0x00200000
THUNK = 0x7F000000           # fstp dword [RESULT] ; then STOP
RESULT = 0x7F000100
STOP = THUNK + 6
LIMIT = 50_000_000           # instructions per call

DRAW_FNS = (0x481C00, 0x481F20)


def align(v, a=0x1000):
    return (v + a - 1) & ~(a - 1)


class Emu:
    def __init__(self, exe, dll):
        self.mu = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        pe = pefile.PE(exe)
        size = align(pe.OPTIONAL_HEADER.SizeOfImage)
        mu.mem_map(EXE_BASE, size, UC_PROT_ALL)
        mu.mem_write(EXE_BASE, pe.get_memory_mapped_image()[:size])
        self.exe_data = (EXE_BASE + 0x16C000, 0x1265E0)
        d = pefile.PE(dll)
        assert d.OPTIONAL_HEADER.ImageBase == DLL_BASE
        mu.mem_map(DLL_BASE, align(d.OPTIONAL_HEADER.SizeOfImage), UC_PROT_ALL)
        for s in d.sections:
            mu.mem_write(DLL_BASE + s.VirtualAddress, s.get_data())
        self.exp = {e.name.decode(): DLL_BASE + e.address for e in d.DIRECTORY_ENTRY_EXPORT.symbols}
        mu.mem_map(HEAP, HEAP_SIZE, UC_PROT_ALL)
        mu.mem_map(STACK, STACK_SIZE, UC_PROT_ALL)
        mu.mem_map(THUNK, 0x1000, UC_PROT_ALL)
        mu.mem_write(THUNK, bytes([0xD9, 0x1D]) + struct.pack('<I', RESULT) + b'\xF4')
        mu.reg_write(UC_X86_REG_CR0, (mu.reg_read(UC_X86_REG_CR0) | 0x2) & ~0x4)
        mu.reg_write(UC_X86_REG_CR4, mu.reg_read(UC_X86_REG_CR4) | 0x600)
        self.brk = HEAP
        self.draws = []
        mu.hook_add(UC_HOOK_CODE, self._draw_hook, begin=DRAW_FNS[0], end=DRAW_FNS[0])
        mu.hook_add(UC_HOOK_CODE, self._draw_hook, begin=DRAW_FNS[1], end=DRAW_FNS[1])

    def _draw_hook(self, mu, addr, size, user):
        sp = mu.reg_read(UC_X86_REG_ESP)
        ret, *args = struct.unpack('<7I', mu.mem_read(sp, 28))
        self.draws.append((addr, mu.reg_read(UC_X86_REG_ECX), tuple(args)))
        mu.reg_write(UC_X86_REG_ESP, sp + 4 + 0x18)
        mu.reg_write(UC_X86_REG_EAX, 1)
        mu.reg_write(UC_X86_REG_EIP, ret)

    # memory helpers
    def alloc(self, n, al=16):
        self.brk = (self.brk + al - 1) & ~(al - 1)
        a = self.brk
        self.brk += n
        assert self.brk < HEAP + HEAP_SIZE
        return a

    def w8(self, a, v): self.mu.mem_write(a, struct.pack('<B', v & 0xFF))
    def w16(self, a, v): self.mu.mem_write(a, struct.pack('<H', v & 0xFFFF))
    def w32(self, a, v): self.mu.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))
    def wf(self, a, v): self.mu.mem_write(a, struct.pack('<f', v))
    def r32(self, a): return struct.unpack('<I', self.mu.mem_read(a, 4))[0]
    def rf(self, a): return struct.unpack('<f', self.mu.mem_read(a, 4))[0]
    def read(self, a, n): return bytes(self.mu.mem_read(a, n))
    def write(self, a, b): self.mu.mem_write(a, b)

    def call(self, addr, args=(), ecx=0, edx=0, fret=False):
        mu = self.mu
        sp = STACK + STACK_SIZE - 0x1000
        frame = struct.pack('<I', THUNK if fret else STOP) + b''.join(struct.pack('<I', a & 0xFFFFFFFF) for a in args)
        sp -= len(frame)
        mu.mem_write(sp, frame)
        mu.reg_write(UC_X86_REG_ESP, sp)
        mu.reg_write(UC_X86_REG_ECX, ecx)
        mu.reg_write(UC_X86_REG_EDX, edx)
        mu.reg_write(UC_X86_REG_FPCW, 0x27F)
        mu.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        mu.emu_start(addr, STOP, count=LIMIT)
        if mu.reg_read(UC_X86_REG_EIP) != STOP:
            raise RuntimeError('call 0x%x did not return (eip 0x%x)' % (addr, mu.reg_read(UC_X86_REG_EIP)))
        if fret:
            return self.read(RESULT, 4)
        return mu.reg_read(UC_X86_REG_EAX)


# ---- synthetic terrain ------------------------------------------------------

NODE_SIZE = 0x60
DATA_SIZE = 0x200
VINFO_SIZE = 0xD60


class World:
    def __init__(self, emu, rng, levels=2, fine_shift=3, kid_density=0.85):
        self.e = e = emu
        self.rng = rng
        self.T = T = e.alloc(0xD00)
        e.write(T, bytes(0xD00))
        e.w32(T + 0x6C, 1)                    # tolerance
        focal = rng.choice([655, 786, 1310])
        e.wf(T + 0x70, focal)
        e.wf(T + 0x74, 1.0 / focal)
        e.wf(T + 0x78, 1.0 / (focal * focal))
        e.w32(T + 0xB4, rng.randrange(1, 1000))
        e.w32(T + 0xCB0, rng.randrange(2))
        self.nodes = []
        self.root = self.make(None, levels, 0, 0, fine_shift + 4 * levels, 0, kid_density)
        e.w32(T + 0x44, self.root)
        self.size = 16 << (fine_shift + 4 * levels)

    def make(self, parent, level, gx, gy, shift, idx, density):
        e, rng = self.e, self.rng
        n = e.alloc(NODE_SIZE)
        D = e.alloc(DATA_SIZE)
        vi = e.alloc(VINFO_SIZE)
        e.write(n, bytes(NODE_SIZE))
        e.write(D, bytes(DATA_SIZE))
        e.w32(n + 0x00, 0x558F0C)
        e.w32(n + 0x08, parent or 0)
        e.w32(n + 0x0C, vi)
        s10 = rng.choice([0.0625, 0.05, 0.1])
        s14 = rng.choice([1.0, 0.5, 2.0])
        e.wf(n + 0x10, s10)
        e.wf(n + 0x14, s14)
        e.w16(n + 0x20, rng.randrange(0, 4000))
        e.w16(n + 0x24, gx)
        e.w16(n + 0x26, gy)
        e.w8(n + 0x28, level)
        e.w8(n + 0x29, shift)
        e.w8(n + 0x2A, idx)
        e.w32(n + 0x34, self.T)
        e.w32(n + 0x38, D)
        # vertex info: band byte, height; sorted order and band counts
        base = rng.randrange(0, 20000)
        imps = []
        buf = bytearray(VINFO_SIZE)
        hmin, hmax = 1e30, -1e30
        rough = rng.choice([200, 2000, 12000])
        for v in range(289):
            x, y = v % 17, v // 17
            imp = rng.randrange(256)
            if (x in (0, 16) and y in (0, 16)) or (x == 8 and y == 8):
                imp = 255
            h = max(0, min(65535, base + rng.randrange(-rough, rough)))
            hy = ((h - base) * s10 + base) * s14
            hmin, hmax = min(hmin, hy), max(hmax, hy)
            struct.pack_into('<BBHBBBB', buf, v * 8, rng.randrange(256), imp, h, rng.randrange(256),
                             rng.randrange(256), rng.randrange(256), 0)
            imps.append(imp)
        order = sorted(range(289), key=lambda v: (imps[v], v))
        for k in range(256):
            struct.pack_into('<h', buf, 0x908 + 2 * k, sum(1 for i in imps if i < k))
        for i, v in enumerate(order):
            struct.pack_into('<h', buf, 0xB08 + 2 * i, v)
        struct.pack_into('<i', buf, 0xD4C, base)
        e.write(vi, bytes(buf))
        e.wf(n + 0x18, hmin)
        e.wf(n + 0x1C, hmax)
        cs = 1 << shift
        e.wf(D + 0x164, gx * cs)
        e.wf(D + 0x168, gy * cs)
        e.wf(D + 0x16C, (gx + 16) * cs)
        e.wf(D + 0x170, (gy + 16) * cs)
        e.wf(D + 0x174, rng.choice([1.0, 0.75, 1.5, 2.0]))
        # mesh for the draw walk: 16 sub-blocks
        nv = rng.randrange(0, 300)
        verts = 0x50000000 + rng.randrange(1 << 20) * 32   # never read (draws are hooked)
        idxb = 0x58000000 + rng.randrange(1 << 20) * 2
        tab = e.alloc(16 * 12)
        cv = ci = 0
        for s in range(16):
            cv += rng.randrange(0, 20)
            ci += rng.randrange(0, 60)
            e.write(tab + s * 12, struct.pack('<hhff', cv, ci, 0.0, 0.0))
        e.w32(D + 0x134, verts)
        e.w32(D + 0x138, idxb)
        e.w16(D + 0x13C, rng.choice([0, nv]))
        e.w16(D + 0x13E, rng.randrange(0, 600))
        e.w16(D + 0x178, rng.randrange(0x10000))
        e.w32(D + 0x17C, tab if rng.random() < 0.9 else 0)
        self.nodes.append(n)
        if level > 0 and (parent is None or rng.random() < density):
            kids = e.alloc(0x400)
            e.write(kids, bytes(0x400))
            e.w32(n + 0x04, kids)
            # a contiguous block of children (so neighbours exist), sometimes all
            if parent is None:
                x0, y0 = rng.randrange(0, 6), rng.randrange(0, 6)
                x1, y1 = x0 + rng.randrange(6, 11), y0 + rng.randrange(6, 11)
            elif rng.random() < 0.3:
                x0, y0, x1, y1 = 0, 0, 16, 16
            else:
                x0, y0 = rng.randrange(0, 12), rng.randrange(0, 12)
                x1, y1 = min(16, x0 + rng.randrange(2, 9)), min(16, y0 + rng.randrange(2, 9))
            if level == 1 and parent is not None and rng.random() < 0.6:
                x1, y1 = x0 + 1, y0 + 1   # keep the tree small
            for cy in range(y0, y1):
                for cx in range(x0, x1):
                    c = self.make(n, level - 1, (gx + cx) * 16, (gy + cy) * 16, shift - 4, cy * 16 + cx,
                                  density * 0.12)
                    e.w32(kids + 4 * (cy * 16 + cx), c)
        return n

    def kids(self, n):
        k = self.e.r32(n + 4)
        if not k:
            return []
        raw = struct.unpack('<256I', self.e.read(k, 0x400))
        return [c for c in raw if c]

    def randomise_state(self):
        e, rng = self.e, self.rng
        for n in self.nodes:
            D = e.r32(n + 0x38)
            f = 0
            if n != self.root and e.r32(n + 4) and rng.random() < 0.3:
                f |= 1
            if rng.random() < 0.05:
                f |= 8
            e.w8(D + 0x121, f)

    def set_vis(self):
        e, rng = self.e, self.rng
        for n in self.nodes:
            r = rng.random()
            if r < 0.15:
                v = 0
            elif r < 0.55:
                v = 0x3FFFFF
            elif r < 0.65:
                v = 0x200000 | rng.randrange(0x200000)
            elif r < 0.7:
                v = 0x200000
            else:
                v = rng.randrange(1, 0x200000) | 1
            e.w32(n + 0x40, v)
        e.w32(self.root + 0x40, rng.choice([0x3FFFFF, 0x1FFFFF, 0x200000 | 0x1FFFFF]))


def flt(b):
    return struct.unpack('<f', b)[0]


def diff_bytes(a, b, base, limit=8):
    out = []
    for i in range(len(a)):
        if a[i] != b[i]:
            out.append('0x%08x: game %02x ours %02x' % (base + i, a[i], b[i]))
            if len(out) >= limit:
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('dll', nargs='?', default=None)
    ap.add_argument('--trees', type=int, default=4)
    ap.add_argument('--frames', type=int, default=12)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--lodfix', type=int, default=0)
    a = ap.parse_args()
    import os
    dll = a.dll or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'harness.dll')
    e = Emu(a.exe, dll)
    # the game's start-up tables
    e.call(0x461A50)
    e.call(0x47EAB0)
    e.call(0x47F420)
    e.call(e.exp['H_SetOptions'], (a.lodfix, 0, 0))
    if a.lodfix:
        print('note: TerrainLodFix clamp compared as off for the game side (only differs above 65535)')
    fails = 0
    checks = {'detail': 0, 'eval': 0, 'draw': 0, 'draw_calls': 0}
    for t in range(a.trees):
        rng = random.Random(a.seed * 1000 + t)
        e.brk = HEAP
        w = World(e, rng, levels=rng.choice([1, 2, 2, 3]))
        w.randomise_state()
        w.set_vis()
        e.call(0x483650, ecx=w.root)      # the game's full reset
        heap_end = e.brk
        print('tree %d: %d nodes, %.1f MB' % (t, len(w.nodes), (heap_end - HEAP) / 1e6))
        cx, cz = rng.uniform(0, w.size), rng.uniform(0, w.size)
        cy = rng.uniform(-200, 3000)
        for fr in range(a.frames):
            # camera moves, sometimes jumps
            if rng.random() < 0.2:
                cx, cz = rng.uniform(-0.1, 1.1) * w.size, rng.uniform(-0.1, 1.1) * w.size
            cx += rng.uniform(-300, 300)
            cz += rng.uniform(-300, 300)
            cy = max(-500.0, cy + rng.uniform(-100, 100))
            e.wf(w.T + 0x60, cx)
            e.wf(w.T + 0x64, cy)
            e.wf(w.T + 0x68, cz)
            e.w32(w.T + 0xB4, e.r32(w.T + 0xB4) + 1)
            w.set_vis()
            # Detail, per node (flag 0 mostly, 1 sometimes)
            for n in w.nodes:
                D = e.r32(n + 0x38)
                flag = 1 if rng.random() < 0.1 else 0
                sd, st = e.read(D, DATA_SIZE), e.read(w.T, 0xD00)
                rg = e.call(0x482550, (flag,), ecx=n, fret=True)
                gd, gt = e.read(D, DATA_SIZE), e.read(w.T, 0xD00)
                e.write(D, sd)
                e.write(w.T, st)
                ro = e.call(e.exp['H_Detail'], (n, flag), fret=True)
                od, ot = e.read(D, DATA_SIZE), e.read(w.T, 0xD00)
                checks['detail'] += 1
                if rg != ro or gd != od or gt != ot:
                    fails += 1
                    if fails < 20:
                        print('DETAIL MISMATCH tree %d frame %d node 0x%x flag %d: ret game %r ours %r' %
                              (t, fr, n, flag, flt(rg), flt(ro)))
                        for l in diff_bytes(gd, od, D) + diff_bytes(gt, ot, w.T):
                            print('   ', l)
                e.write(D, gd)
                e.write(w.T, gt)
            if fr == 0:
                los = [e.read(e.r32(n + 0x38) + 0x122, 4) for n in w.nodes[:400:40]]
                print('  lo/hi sample:', [struct.unpack('<HH', x) for x in los])
            # Eval, whole tree
            snap = e.read(HEAP, heap_end - HEAP)
            sdat = e.read(*e.exe_data)
            e.call(0x4836E0, ecx=w.root)
            g = e.read(HEAP, heap_end - HEAP)
            gdat = e.read(*e.exe_data)
            e.write(HEAP, snap)
            e.write(e.exe_data[0], sdat)
            e.call(e.exp['H_Eval'], (w.root,))
            o = e.read(HEAP, heap_end - HEAP)
            checks['eval'] += 1
            if g != o:
                fails += 1
                print('EVAL MISMATCH tree %d frame %d' % (t, fr))
                for l in diff_bytes(g, o, HEAP, 16):
                    print('   ', l)
            if gdat != sdat:
                print('note: the game wrote exe data during Eval')
            e.write(HEAP, g)
            # Draw walk
            a1, a6 = rng.randrange(1 << 30), rng.randrange(1 << 30)
            e.draws = []
            e.call(0x4818B0, (a1, 0, 0, 0x10, 0, a6), ecx=w.root)
            gdraws = e.draws
            e.draws = []
            e.call(e.exp['H_Draw'], (w.root, a1, 0, 0, 0x10, 0, a6))
            odraws = e.draws
            checks['draw'] += 1
            checks['draw_calls'] += len(gdraws)
            if gdraws != odraws:
                fails += 1
                print('DRAW MISMATCH tree %d frame %d: game %d calls, ours %d' % (t, fr, len(gdraws), len(odraws)))
                for i, (x, y) in enumerate(zip(gdraws, odraws)):
                    if x != y:
                        print('    first difference at call %d: game %r ours %r' % (i, x, y))
                        break
        st = struct.unpack('<8I', e.read(e.call(e.exp['H_Stats']), 32))
        print('  ours: eval nodes %d tests %d, draw blocks %d calls %d' % (st[4], st[5], st[6], st[7]))
    print('checks: %r; mismatches: %d' % (checks, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
