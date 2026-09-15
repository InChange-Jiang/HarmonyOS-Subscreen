#!/usr/bin/env python3
"""核对 --dump 导出的 NV12 里的 Y/Cb/Cr 是否等于按 BT.601 limited 应该算出的值。

用法: python check_chroma.py <前缀>

对若干采样点，同时给出:
  * 正确算法(算术右移, 等价于 C 里带符号的 >>)算出的 Y/Cb/Cr
  * 若把负数用"逻辑右移"处理(SSE _mm_srli_epi32 的行为)会得到什么
  * 文件里实际写着的 Y/Cb/Cr
用来定位 BGRA->NV12 到底哪一步算错了。
"""
import sys

base = sys.argv[1] if len(sys.argv) > 1 else "dump"
meta = open(base + ".txt").read().split()
sw, sh = int(meta[0]), int(meta[1])
fw, fh = int(meta[2]), int(meta[3])

bgra = open(base + ".bgra", "rb").read()
nv = open(base + ".nv12", "rb").read()
Yp = nv[: sw * sh]
UVp = nv[sw * sh:]


def arith(x, n=8):
    """C 里带符号整数的 >> (算术右移, 向负无穷取整)"""
    return x >> n


def logical32(x, n=8):
    """SSE _mm_srli_epi32: 把 32 位当无符号右移, 再按 packus 饱和到 65535 / 255"""
    u = (x + (1 << 32)) % (1 << 32) if x < 0 else x
    u >>= n
    u += 128
    if u > 65535:
        u = 65535
    if u > 255:
        u = 255
    return u


def enc_ok(R, G, B):
    y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16
    u = (arith(-38 * R - 74 * G + 112 * B + 128) ) + 128
    v = (arith(112 * R - 94 * G - 18 * B + 128)) + 128
    return y, u, v


def enc_sse_u(R, G, B):
    """U 用逻辑右移(复现 SSE 的行为), V 用正确的算术右移"""
    y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16
    u = logical32(-38 * R - 74 * G + 112 * B + 128)
    v = (arith(112 * R - 94 * G - 18 * B + 128)) + 128
    return y, u, v


print("采样点 (stream %dx%d, bgra %dx%d)" % (sw, sh, fw, fh))
print("%-14s %-16s %-18s %-18s %-18s" % ("坐标", "BGRA(R,G,B)", "正确YUV", "若U用逻辑右移", "文件实际YUV"))
print("-" * 92)
pts = [(200, 200), (600, 400), (1200, 800), (1800, 300), (2300, 900), (400, 1300), (2500, 1500), (1280, 60)]
for (x, y) in pts:
    if x >= fw or y >= fh:
        continue
    p = (y * fw + x) * 4
    B, G, R = bgra[p], bgra[p + 1], bgra[p + 2]
    ok = enc_ok(R, G, B)
    sse = enc_sse_u(R, G, B)
    ay = Yp[y * sw + x]
    au = UVp[(y // 2) * sw + (x // 2) * 2]
    av = UVp[(y // 2) * sw + (x // 2) * 2 + 1]
    print("%-14s %-16s %-18s %-18s %-18s"
          % ("(%d,%d)" % (x, y), "(%3d,%3d,%3d)" % (R, G, B),
             "(%3d,%3d,%3d)" % ok, "(%3d,%3d,%3d)" % sse, "(%3d,%3d,%3d)" % (ay, au, av)))

print()
print("说明: 若'文件实际YUV'与'若U用逻辑右移'吻合, 就证明 U 的负值被逻辑右移算坏了。")
