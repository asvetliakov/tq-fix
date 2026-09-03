import struct, sys
class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        self.lfa = struct.unpack_from("<I", d, 0x3c)[0]
        assert d[self.lfa:self.lfa+4] == b"PE\0\0"
        nsec = struct.unpack_from("<H", d, self.lfa + 6)[0]
        optsize = struct.unpack_from("<H", d, self.lfa + 20)[0]
        self.opt = self.lfa + 24
        self.base = struct.unpack_from("<I", d, self.opt + 28)[0]
        self.imagesize = struct.unpack_from("<I", d, self.opt + 56)[0]
        self.sections = []
        off = self.opt + optsize
        for _ in range(nsec):
            name = d[off:off+8].rstrip(b"\0").decode()
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, off + 8)
            self.sections.append((name, va, vsize, raw, rawsize)); off += 40
    def off(self, rva):
        for name, va, vsize, raw, rawsize in self.sections:
            if va <= rva < va + max(vsize, rawsize): return raw + (rva - va)
        raise SystemExit("rva %#x unmapped" % rva)
    def read(self, va, n):
        return self.data[self.off(va - self.base if va >= self.base else va):][:n]
    def exports(self):
        er = struct.unpack_from("<I", self.data, self.opt + 96)[0]
        o = self.off(er)
        f = struct.unpack_from("<IIIIIIIIII", self.data, o)
        nname, frva, nrva, orva = f[6], f[7], f[8], f[9]
        fo, no, oo = self.off(frva), self.off(nrva), self.off(orva)
        t = {}
        for i in range(nname):
            p = struct.unpack_from("<I", self.data, no + 4*i)[0]
            s = self.data[self.off(p):]
            nm = s[:s.index(b"\0")].decode("latin-1")
            t[nm] = struct.unpack_from("<I", self.data,
                     fo + 4*struct.unpack_from("<H", self.data, oo + 2*i)[0])[0]
        return t
    def imports(self):
        ir = struct.unpack_from("<I", self.data, self.opt + 104)[0]
        o = self.off(ir); out = {}
        while True:
            oft, _, _, name, first = struct.unpack_from("<IIIII", self.data, o)
            if not (oft or first): break
            s = self.data[self.off(name):]; dll = s[:s.index(b"\0")].decode("latin-1")
            t = self.off(oft or first); i = 0
            while True:
                e = struct.unpack_from("<I", self.data, t + 4*i)[0]
                if not e: break
                if e & 0x80000000: nm = "#%d" % (e & 0xffff)
                else:
                    ss = self.data[self.off(e) + 2:]; nm = ss[:ss.index(b"\0")].decode("latin-1")
                out[self.base + first + 4*i] = (dll, nm); i += 1
            o += 20
        return out
