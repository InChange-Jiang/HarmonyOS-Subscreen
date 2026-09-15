#!/usr/bin/env python3
"""假接收端 —— 在没有平板时验证 PC 发送端。

在 PC 本地起一个 TCP 服务，按协议解析 [4字节大端长度][payload]，
统计收帧数 / 码率 / 是否见到 SPS/PPS/IDR，然后打印汇总。

用法:
    python fake_receiver.py [port] [seconds]
默认 port=53518 seconds=15

配合发送端使用（注意用 --nohdc 避免它去建 fport）:
    subscreen_sender.exe --nohdc --port 53518 --vdd 1920x1080@60 --duration 8
"""
import json
import socket
import struct
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 53518
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
srv.settimeout(seconds)
print("[fake] 监听 127.0.0.1:%d，等待 %gs" % (port, seconds), flush=True)

try:
    conn, _ = srv.accept()
except socket.timeout:
    print("[fake] 超时：没有任何连接", flush=True)
    sys.exit(2)

print("[fake] 已连接", flush=True)
conn.settimeout(1.0)

buf = bytearray()
t0 = time.time()
deadline = t0 + seconds
hello = None
frames = 0
total = 0
nal_types = set()
first_frame_at = None
last_report = t0

try:
    while time.time() < deadline:
        try:
            chunk = conn.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            print("[fake] 对端关闭连接", flush=True)
            break
        buf.extend(chunk)

        # 拆包
        pos = 0
        while len(buf) - pos >= 4:
            n = struct.unpack_from(">I", buf, pos)[0]
            if n > 4 * 1024 * 1024:
                print("[fake] 协议错误：长度 %d 异常" % n, flush=True)
                raise SystemExit(3)
            if len(buf) - pos - 4 < n:
                break
            payload = bytes(buf[pos + 4:pos + 4 + n])
            pos += 4 + n

            if hello is None:
                hello = payload.decode("utf-8", "replace")
                print("[fake] 握手: %s" % hello, flush=True)
                try:
                    hv = json.loads(hello)
                    print("[fake] 流参数: %dx%d" % (hv["width"], hv["height"]), flush=True)
                except Exception:
                    pass
            else:
                frames += 1
                total += n
                if first_frame_at is None:
                    first_frame_at = time.time()
                # Annex-B: 扫 00 00 01 后的 nal_unit_type
                i = 0
                lim = min(len(payload), 4096)
                while i + 4 < lim:
                    if payload[i] == 0 and payload[i + 1] == 0 and payload[i + 2] == 1:
                        nal_types.add(payload[i + 3] & 0x1F)
                        i += 4
                    else:
                        i += 1

        if pos > 0:
            del buf[:pos]

        now = time.time()
        if now - last_report >= 1.0:
            el = now - t0
            print("[fake] %.1fs  帧=%d  累计=%.2fMB  均码率=%.2fMbps"
                  % (el, frames, total / 1048576.0, total * 8 / el / 1e6), flush=True)
            last_report = now
finally:
    conn.close()
    srv.close()

el = time.time() - t0
print("-" * 56)
print("[fake] 结果: 帧=%d 字节=%.2fMB 时长=%.1fs" % (frames, total / 1048576.0, el))
if first_frame_at:
    span = max(time.time() - first_frame_at, 1e-6)
    print("[fake] 稳定期码率 ≈ %.2f Mbps  (首帧后 %.1fs)" % (total * 8 / span / 1e6, span))
print("[fake] 见到 NAL 类型: %s"
      % sorted("type%d" % t for t in nal_types))
if 7 in nal_types and 8 in nal_types:
    print("[fake] ✓ 含 SPS(7)/PPS(8)，接收端能初始化解码器")
else:
    print("[fake] ✗ 缺 SPS/PPS，平板会黑屏！")
if 5 in nal_types:
    print("[fake] ✓ 含 IDR(5)，有完整关键帧")
else:
    print("[fake] ✗ 未见 IDR(5)，平板可能一直黑屏")

sys.exit(0 if frames > 0 else 1)
