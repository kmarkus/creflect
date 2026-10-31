#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
#
"""Generate a header with random C types for the compiler oracle.

usage: randtypes.py <seed> [ntypes]

Covers combinations hand-written testcases miss: bitfields of all
integer types and widths, zero width and unnamed bitfields, packed
and aligned structs and members, unions, enums, nesting, anonymous
members, arrays and pointers. Same seed, same header.
"""

import random
import sys

INTS = [
    ("char", 8), ("signed char", 8), ("unsigned char", 8),
    ("short", 16), ("unsigned short", 16), ("int", 32), ("unsigned", 32),
    ("long long", 64), ("unsigned long long", 64),
    ("int8_t", 8), ("uint8_t", 8), ("int16_t", 16), ("uint16_t", 16),
    ("int32_t", 32), ("uint32_t", 32), ("int64_t", 64), ("uint64_t", 64),
]
# width of long depends on the ABI, only used as non-bitfield
SCALARS = [t for t, _ in INTS] + [
    "long", "unsigned long", "_Bool", "bool", "float", "double", "long double",
    "size_t", "intptr_t",
]
ALIGNS = [1, 2, 4, 8, 16]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.p = f"r{seed}_"
        self.out = []
        self.types = []         # complete struct/union/typedef names
        self.enums = []         # (name, bits needed, signed)
        self.n = 0

    def name(self, what):
        self.n += 1
        return f"{self.p}{what}{self.n}"

    def chance(self, p):
        return self.r.random() < p

    def enum(self):
        name = self.name("e")
        vals, v = [], 0
        neg = self.chance(0.3)
        big = self.chance(0.1)
        for _ in range(self.r.randint(1, 5)):
            if self.chance(0.4):
                v = self.r.randint(-100 if neg else 0, 300)
                if big and self.chance(0.3):
                    v = self.r.choice([1 << 33, -(1 << 33) if neg else 1 << 40])
            vals.append((self.name("E"), v))
            v += 1
        lo, hi = min(x for _, x in vals), max(x for _, x in vals)
        signed = lo < 0
        bits = 1
        while not (-(1 << (bits - 1)) <= lo and hi < (1 << (bits - 1)) if signed
                   else hi < (1 << bits)):
            bits += 1
        packed = self.chance(0.2)
        attr = " __attribute__((packed))" if packed else ""
        body = ", ".join(f"{n} = {x}" for n, x in vals)
        self.out.append(f"enum {name} {{ {body} }}{attr};\n")
        size = 64 if hi > 0xffffffff or lo < -0x80000000 else 32
        self.enums.append((f"enum {name}", bits, size, packed))

    def member_type(self, depth):
        """returns (type spec, declarator suffix/prefix fn)"""
        k = self.r.random()
        if k < 0.55 or depth > 2:
            return self.r.choice(SCALARS)
        if k < 0.65 and self.enums:
            return self.r.choice(self.enums)[0]
        if k < 0.85 and self.types:
            return self.r.choice(self.types)
        return None     # inline anonymous composite

    def bitfield(self, body):
        if self.enums and self.chance(0.15):
            t, bits, size, packed = self.r.choice(self.enums)
            if packed:      # small underlying type, keep it simple
                return
            w = self.r.randint(bits, size)
        else:
            t, size = self.r.choice(INTS + [("_Bool", 1)])
            w = self.r.randint(0, size)
        attr = " __attribute__((packed))" if self.chance(0.05) else ""
        if w == 0 or self.chance(0.1):
            body.append(f"{t} : {w}{attr};")
        else:
            body.append(f"{t} {self.name('b')} : {w}{attr};")

    def composite(self, depth, anon=False):
        kind = "union" if self.chance(0.2) else "struct"
        body = []
        named = 0
        for _ in range(self.r.randint(1, 7)):
            if self.chance(0.35):
                self.bitfield(body)
                continue
            t = self.member_type(depth)
            if t is None:
                inner = self.composite(depth + 1, anon=True)
                if self.chance(0.4):
                    body.append(f"{inner};")      # anonymous member
                    named += 1
                    continue
                t = inner
            decl = self.name("m")
            if self.chance(0.15):
                decl = "*" + decl
            for _ in range(self.r.choice([0, 0, 0, 1, 1, 2])):
                decl += f"[{self.r.randint(1, 3)}]"
            attrs = ""
            if self.chance(0.07):
                attrs += " __attribute__((packed))"
            if self.chance(0.07):
                attrs += f" __attribute__((aligned({self.r.choice(ALIGNS)})))"
            body.append(f"{t} {decl}{attrs};")
            named += 1
        if named == 0:
            body.append(f"char {self.name('m')};")

        attrs = ""
        if self.chance(0.25):
            attrs += " __attribute__((packed))"
        if self.chance(0.08):
            attrs += f" __attribute__((aligned({self.r.choice(ALIGNS)})))"
        text = "{ " + " ".join(body) + " }" + attrs
        if anon:
            return f"{kind} {text}"

        if self.chance(0.2):
            name = self.name("t")
            self.out.append(f"typedef {kind} {text} {name};\n")
            self.types.append(name)
        else:
            name = f"{kind} {self.name('s')}"
            self.out.append(f"{name} {text};\n")
            self.types.append(name)

    def run(self, ntypes):
        self.out.append("#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n\n")
        for _ in range(ntypes):
            if self.chance(0.2):
                self.enum()
            else:
                self.composite(0)
        return "".join(self.out)


def main():
    seed = int(sys.argv[1])
    ntypes = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    sys.stdout.write(Gen(seed).run(ntypes))


if __name__ == "__main__":
    main()
