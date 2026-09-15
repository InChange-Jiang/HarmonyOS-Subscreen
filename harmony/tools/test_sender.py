#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
副屏测试推流器（PC 端）— 对应《鸿蒙平板有线副屏技术方案》阶段一/二验证

用法:
  1. 生成测试视频（需要 ffmpeg）:
     ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 60 \
            -c:v libx264 -profile:v baseline -tune zerolatency -g 60 -bf 0 \
            -pix_fmt yuv420p -f h264 test.h264
  2. 建立端口转发（需要 hdc，平板开启 USB 调试并连接）:
     hdc fport tcp:53517 tcp:53517
  3. 推流:
     python test_sender.py --file test.h264 --width 1920 --height 1080 --fps 30 [--loop]

协议（与鸿蒙端 subscreen NDK 实现一致）:
  [4字节大端长度][payload]
  第一个包必须是握手 JSON: {"type":"hello","width":W,"height":H}
  之后每包为一个 H.264 访问单元（首包必须含 SPS+PPS+IDR）
"""

import argparse
import socket
import struct
import sys
import time


def find_nals(data: bytes):
    """按 Annex-B 起始码切出 NAL 单元（含起始码）。"""
    nals = []
    i = 0
    n = len(data)
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, 4))
                i += 3
                continue
            if i < n - 4 and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, 5))
                i += 4
                continue
        i += 1
    for idx, (pos, sc) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else n
        nals.append(data[pos:end])
    return nals


def nal_type(nal: bytes) -> int:
    for i in range(len(nal)):
        if i >= 1 and nal[i - 1] == 0 and nal[i] == 1:
            return nal[i + 1] & 0x1F
        if i >= 2 and nal[i - 2] == 0 and nal[i - 1] == 0 and nal[i] == 1:
            return nal[i + 1] & 0x1F
    # 无起始码前缀的裸 NAL
    return (nal[0] & 0x1F) if nal else -1


def has_slice(au: bytes) -> bool:
    return any(nal_type(n) in (1, 5) for n in find_nals(au))


def split_access_units(data: bytes):
    """把 NAL 序列组合成访问单元：切片(1/5)开帧，SPS/PPS/SEI/AUD 挂到下一帧。"""
    aus = []
    cur = bytearray()
    for nal in find_nals(data):
        t = nal_type(nal)
        if t in (1, 5):  # slice
            if cur and has_slice(bytes(cur)):
                aus.append(bytes(cur))
                cur = bytearray()
            cur += nal
        elif t in (6, 7, 8, 9):  # SEI / SPS / PPS / AUD
            if cur and has_slice(bytes(cur)):
                aus.append(bytes(cur))
                cur = bytearray()
            cur += nal
        else:
            cur += nal
    if cur:
        aus.append(bytes(cur))
    return aus


def send_packet(sock: socket.socket, payload: bytes):
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def main():
    ap = argparse.ArgumentParser(description="副屏 H.264 测试推流器")
    ap.add_argument("--file", required=True, help="Annex-B 裸 H.264 文件")
    ap.add_argument("--host", default="127.0.0.1", help="目标地址（经 hdc fport 转发）")
    ap.add_argument("--port", type=int, default=53517)
    ap.add_argument("--width", type=int, required=True, help="视频宽度（握手用）")
    ap.add_argument("--height", type=int, required=True, help="视频高度（握手用）")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--loop", action="store_true", help="循环播放")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        data = f.read()
    aus = split_access_units(data)
    if not aus:
        print("未解析到访问单元", file=sys.stderr)
        sys.exit(1)
    print(f"载入 {len(aus)} 个访问单元, 共 {len(data) / 1024 / 1024:.1f} MB")

    sock = socket.create_connection((args.host, args.port), timeout=5)
    hello = f'{{"type":"hello","width":{args.width},"height":{args.height}}}'.encode()
    send_packet(sock, hello)
    print(f"已连接 {args.host}:{args.port}, 握手 {args.width}x{args.height}")

    interval = 1.0 / args.fps
    seq = 0
    t0 = time.perf_counter()
    bytes_sent = 0
    try:
        while True:
            for au in aus:
                send_packet(sock, au)
                seq += 1
                bytes_sent += len(au) + 4
                if seq % 300 == 0:
                    dt = time.perf_counter() - t0
                    print(f"已发 {seq} 帧, {bytes_sent / 1024 / 1024:.1f} MB, "
                          f"平均 {seq / dt:.1f} fps, {bytes_sent * 8 / dt / 1e6:.1f} Mbps")
                target = seq * interval
                delay = t0 + target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
            if not args.loop:
                break
    except (BrokenPipeError, ConnectionResetError):
        print("对端断开")
    finally:
        sock.close()
        dt = time.perf_counter() - t0
        print(f"结束: {seq} 帧, {bytes_sent / 1024 / 1024:.1f} MB, "
              f"{seq / max(dt, 1e-6):.1f} fps, {bytes_sent * 8 / max(dt, 1e-6) / 1e6:.1f} Mbps")


if __name__ == "__main__":
    main()
