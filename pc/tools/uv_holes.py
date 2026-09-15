#!/usr/bin/env python3
"""分析 --dump 导出的 NV12 里 UV 平面的"空洞"规律。

用法: python uv_holes.py <前缀>
"""
import sys

base = sys.argv[1] if len(sys.argv) > 1 else "dump"
meta = open(base + ".txt").read().split()
sw, sh = int(meta[0]), int(meta[1])
nv = open(base + ".nv12", "rb").read()
UV = nv[sw * sh:]

print("UV 平面大小 = %d 字节 (期望 %d)" % (len(UV), sw * (sh // 2)))
zero = sum(1 for c in UV if c == 0)
print("零字节数 = %d / %d  (%.1f%%)" % (zero, len(UV), 100.0 * zero / len(UV)))
print()

# 按"UV 行内偏移 mod 16"统计零的分布 —— 能看出是不是 16 字节块的覆盖问题
buckets = [0] * 16
tot = [0] * 16
for i, c in enumerate(UV):
    m = i % 16
    tot[m] += 1
    if c == 0:
        buckets[m] += 1
print("UV 平面内偏移 mod 16 的零值比例:")
for m in range(16):
    print("  mod16=%2d : %5.1f%%  (%d/%d)" % (m, 100.0 * buckets[m] / tot[m], buckets[m], tot[m]))
print()

# 打印若干 UV 行前 64 字节, 看空洞排布
for row in (30, 100, 200, 450):
    off = row * sw
    seg = UV[off:off + 64]
    print("UV 行 %4d 前 64 字节:" % row)
    print("   " + " ".join("%02x" % b for b in seg))
