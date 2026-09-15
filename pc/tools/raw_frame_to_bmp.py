#!/usr/bin/env python3
"""把 --dump 导出的一帧还原成 PNG，用于离线判断"颜色到底在哪一段变坏的"。

用法:
    python raw_frame_to_bmp.py <前缀> [step]

输入(由 subscreen_sender.exe --dump <前缀> 生成):
    <前缀>.txt   "推流宽 推流高 桌面宽 桌面高"
    <前缀>.nv12  送进编码器的 NV12 原始数据
    <前缀>.bgra  转换之前的 BGRA 源数据(含鼠标合成)

输出:
    <前缀>_bgra.png   源画面
    <前缀>_nv12.png   把 NV12 按 BT.601 limited 解回来 —— 应当与源画面一致

两者若有色差，说明发送端自己的转换有问题；若一致，说明问题在编码器/平板解码器。
直接写 PNG(zlib 是标准库)，避免依赖 Pillow / ImageMagick。
"""
import os
import struct
import sys
import zlib


def write_png(path, w, h, rows):
    """rows: 每行 w*3 字节的 RGB，顺序自图像顶部向下。"""
    raw = bytearray()
    for r in rows:
        raw.append(0)   # 每个扫描线前的滤波器字节: 0 = None
        raw.extend(r)
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8bit, truecolor RGB
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def main():
    if len(sys.argv) < 2:
        print("用法: python raw_frame_to_bmp.py <前缀> [step]")
        return 2
    base = sys.argv[1]
    step = int(sys.argv[2]) if len(sys.argv) > 2 else 2

    meta = open(base + ".txt").read().split()
    sw, sh = int(meta[0]), int(meta[1])   # 推流尺寸
    fw, fh = int(meta[2]), int(meta[3])   # 桌面尺寸(BGRA)

    # ---- 1) 源 BGRA -> RGB ----
    # GPU 路径(--size)下程序不回读 BGRA, 这时没有 .bgra 文件, 跳过即可。
    if os.path.exists(base + ".bgra"):
        bgra = open(base + ".bgra", "rb").read()
        ow, oh = fw // step, fh // step
        rows = []
        for y in range(0, fh, step):
            off = y * fw * 4
            row = bytearray(ow * 3)
            for i, x in enumerate(range(0, fw, step)):
                p = off + x * 4
                row[i * 3] = bgra[p + 2]      # R
                row[i * 3 + 1] = bgra[p + 1]  # G
                row[i * 3 + 2] = bgra[p]      # B
            rows.append(bytes(row))
        write_png(base + "_bgra.png", ow, oh, rows)
        print("源 BGRA  ->", base + "_bgra.png", "%dx%d" % (ow, oh))
    else:
        print("(没有 .bgra, 跳过源图；GPU 路径不回读 BGRA 属正常)")

    # ---- 2) NV12 -> RGB (BT.601 limited) ----
    # NV12 的尺寸是"推流尺寸"(可能已被 --size / --scale 缩小), 与 BGRA 的桌面尺寸无关
    ow, oh = sw // step, sh // step
    nv = open(base + ".nv12", "rb").read()
    Y = nv[: sw * sh]
    UV = nv[sw * sh:]
    rows = []
    for y in range(0, sh, step):
        row = bytearray(ow * 3)
        ybase = y * sw
        uvbase = (y // 2) * sw
        for i, x in enumerate(range(0, sw, step)):
            c = Y[ybase + x] - 16
            if c < 0:
                c = 0
            d = UV[uvbase + (x // 2) * 2] - 128
            e = UV[uvbase + (x // 2) * 2 + 1] - 128
            row[i * 3] = clamp((298 * c + 409 * e + 128) >> 8)
            row[i * 3 + 1] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8)
            row[i * 3 + 2] = clamp((298 * c + 516 * d + 128) >> 8)
        rows.append(bytes(row))
    write_png(base + "_nv12.png", ow, oh, rows)
    print("NV12     ->", base + "_nv12.png", "%dx%d" % (ow, oh))
    return 0


sys.exit(main())
