#!/usr/bin/env python3
"""Ferramentas para os testes: cria um registro (hive) minimo e converte a lista de
subchaves de ControlSet001\\Services para 'ri' (como o Windows faz em chaves grandes)."""
import struct, sys
def cks(b):
    c = 0
    for i in range(0, 508, 4): c ^= struct.unpack_from('<I', b, i)[0]
    struct.pack_into('<I', b, 0x1FC, c)
def minimal(out):
    hb = bytearray(4096); hb[0:4] = b'hbin'; struct.pack_into('<II', hb, 4, 0, 4096)
    nk = bytearray(0x50); nk[0:2] = b'nk'; struct.pack_into('<H', nk, 2, 0x2C)
    for o, v in [(0x10, 0), (0x14, 0), (0x18, 0), (0x1C, 0xFFFFFFFF), (0x20, 0xFFFFFFFF), (0x24, 0), (0x28, 0xFFFFFFFF), (0x2C, 0x78), (0x30, 0xFFFFFFFF)]:
        struct.pack_into('<I', nk, o, v)
    struct.pack_into('<H', nk, 0x48, 4); nk[0x4C:0x50] = b'ROOT'
    c = struct.pack('<i', -0x58) + nk; hb[0x20:0x20 + len(c)] = c
    sd = struct.pack('<BBHIIII', 1, 0, 0x8004, 0, 0, 0, 0)
    sk = b'sk\0\0' + struct.pack('<IIII', 0x78, 0x78, 1, len(sd)) + sd
    c = struct.pack('<i', -48) + sk; c += b'\0' * (48 - len(c)); hb[0x78:0x78 + 48] = c
    struct.pack_into('<i', hb, 0xA8, 4096 - 0xA8)
    base = bytearray(4096); base[0:4] = b'regf'; struct.pack_into('<IIQIIIIIII', base, 4, 1, 1, 0, 1, 3, 0, 1, 0x20, 4096, 1); cks(base)
    open(out, 'wb').write(base + hb)
def to_ri(src, out):
    b = bytearray(open(src, 'rb').read())
    cell = lambda off: 0x1000 + off + 4
    def name(off):
        d = cell(off); nl = struct.unpack_from('<H', b, d + 0x48)[0]; return b[d + 0x4C:d + 0x4C + nl].decode('latin1')
    def subs(off):
        d = cell(off); lst = struct.unpack_from('<I', b, d + 0x1C)[0]; l = cell(lst); cnt = struct.unpack_from('<H', b, l + 2)[0]
        return [struct.unpack_from('<II', b, l + 4 + 8 * i) for i in range(cnt)]
    root = struct.unpack_from('<I', b, 0x24)[0]
    cs = [o for o, h in subs(root) if name(o) == 'ControlSet001'][0]
    svc = [o for o, h in subs(cs) if name(o) == 'Services'][0]
    ents = subs(svc); half = len(ents) // 2; hoff = len(b) - 0x1000
    def lh(es):
        d = b'lh' + struct.pack('<H', len(es)) + b''.join(struct.pack('<II', o, h) for o, h in es)
        sz = (len(d) + 4 + 7) // 8 * 8; return struct.pack('<i', -sz) + d + b'\0' * (sz - 4 - len(d))
    A = lh(ents[:half]); B = lh(ents[half:]); oA = hoff + 32; oB = oA + len(A); oR = oB + len(B)
    ri = b'ri' + struct.pack('<H', 2) + struct.pack('<II', oA, oB); rs = (len(ri) + 4 + 7) // 8 * 8
    body = A + B + struct.pack('<i', -rs) + ri + b'\0' * (rs - 4 - len(ri)); total = (32 + len(body) + 4095) // 4096 * 4096
    free = total - 32 - len(body)
    b += b'hbin' + struct.pack('<II', hoff, total) + b'\0' * 20 + body + (struct.pack('<i', free) + b'\0' * (free - 4) if free else b'')
    struct.pack_into('<I', b, cell(svc) + 0x1C, oR); struct.pack_into('<I', b, 0x28, len(b) - 0x1000); cks(b)
    open(out, 'wb').write(b)
if __name__ == '__main__':
    {'minimal': lambda: minimal(sys.argv[2]), 'ri': lambda: to_ri(sys.argv[2], sys.argv[3])}[sys.argv[1]]()
