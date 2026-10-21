#!/usr/bin/env python3
# Parse a .wasm binary and report the largest function bodies.
import sys

data = open(sys.argv[1], 'rb').read()

def leb(o):
    r = 0; s = 0
    while True:
        b = data[o]; o += 1
        r |= (b & 0x7f) << s
        if not (b & 0x80):
            break
        s += 7
    return r, o

assert data[:4] == b'\x00asm', "not a wasm file"
o = 8
num_imported_funcs = 0
code_funcs = []          # (code_index, body_size)
func_names = {}          # global_func_index -> name

while o < len(data):
    sid = data[o]; o += 1
    size, o = leb(o)
    end = o + size
    if sid == 2:                       # import section
        cnt, p = leb(o)
        for _ in range(cnt):
            ml, p = leb(p); p += ml
            nl, p = leb(p); p += nl
            kind = data[p]; p += 1
            if kind == 0x00:
                num_imported_funcs += 1
                _, p = leb(p)
            elif kind == 0x01:
                p += 1
                fl = data[p]; p += 1
                _, p = leb(p)
                if fl & 1: _, p = leb(p)
            elif kind == 0x02:
                fl = data[p]; p += 1
                _, p = leb(p)
                if fl & 1: _, p = leb(p)
            elif kind == 0x03:
                p += 2
    elif sid == 10:                    # code section
        cnt, p = leb(o)
        for i in range(cnt):
            bs, p = leb(p)
            code_funcs.append((i, bs))
            p += bs
    elif sid == 0:                     # custom section
        nl, p = leb(o)
        nm = data[p:p+nl]; p += nl
        if nm == b'name':
            while p < end:
                ssid = data[p]; p += 1
                sssz, p = leb(p)
                ssend = p + sssz
                if ssid == 1:
                    c, p = leb(p)
                    for _ in range(c):
                        idx, p = leb(p)
                        l, p = leb(p)
                        func_names[idx] = data[p:p+l].decode('utf-8', 'replace')
                        p += l
                p = ssend
    o = end

code_funcs.sort(key=lambda t: -t[1])
print(f"imported funcs: {num_imported_funcs}, code funcs: {len(code_funcs)}")
print(f"name section: {'present' if func_names else 'ABSENT (build stripped names)'}")
LIMIT = 7654321
over = [t for t in code_funcs if t[1] > LIMIT]
print(f"functions over the 7.65 MB wasm limit: {len(over)}")
for i, bs in code_funcs[:15]:
    gidx = num_imported_funcs + i
    nm = func_names.get(gidx, '(no name)')
    flag = '  <-- OVER LIMIT' if bs > LIMIT else ''
    print(f"  func #{gidx}  {bs:>10} bytes  ({bs/1048576:6.2f} MB)  {nm}{flag}")
