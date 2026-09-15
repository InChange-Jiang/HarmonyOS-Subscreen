# SubScreen — 鸿蒙平板有线副屏

> 把笔记本**内屏画面**经 USB 线推到鸿蒙平板，当作第二块真实工作区的镜像副屏。
> Windows 端做桌面捕获 + H.264 硬件编码，经 `hdc fport` 的 TCP 通道推给平板端解码渲染。

```
┌─────────────────────┐   USB (hdc fport tcp:53517)   ┌──────────────────────┐
│  Windows 发送端      │ ────────────────────────────▶ │  鸿蒙平板接收端       │
│  DDA 桌面捕获        │   长度前缀帧 + H.264 AUs      │  TCP 收流 → 解码      │
│  D3D11 缩放/色彩转换 │                               │  → Surface 渲染      │
│  QSV / DX12 硬编     │                               │  ArkTS 横屏 UI       │
└─────────────────────┘                               └──────────────────────┘
```

## 目录结构

```
.
├── pc/        # Windows 发送端（C++11 单文件，约 5300 行）
│   ├── subscreen_sender.cpp   # 捕获/编码/推流/GUI/托盘/自愈
│   ├── build.ps1              # 构建脚本（直调 cl.exe）
│   ├── *.cmd                  # 命令行 / GUI 启动入口
│   ├── tools/                 # 诊断与验证脚本（py / ps1）
│   └── third_party/           # parsec-vdd 参考头文件
└── harmony/   # 鸿蒙接收端（DevEco Studio NEXT 工程）
    └── entry/src/main/
        ├── cpp/               # NAPI 入口 + TCP 服务端 + H.264 硬解
        └── ets/               # ArkTS 横屏 UI
```

## 使用流程

1. 平板插 USB + 打开 SubScreen App。
2. PC 双击快捷方式，点「启动副屏」。
3. 窗口收进托盘（绿点 = 推流中），右键托盘可停止/退出。
4. 停止或退出都会自动恢复"仅 PC 单屏"。

## Windows 端构建与运行

- 前提：Visual Studio 2022（MSVC 14.44.x）+ Windows SDK 10.0.26100。
- 构建：`powershell -File build.ps1`，产物为 `subscreen_sender.exe`。
- 关键编译选项（勿随意改动）：`/MT`（静态 CRT）、`/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`（托盘常驻无控制台）、`/arch:AVX2`（色彩转换 SIMD）。
- 运行依赖：`hdc.exe`（随 DevEco Studio 安装，程序自动搜索路径）。
- 入口脚本：
  - `start-mirror.cmd` — 命令行推流（1920×1200@45）
  - `start-mirror-sharp.cmd` — 1:1 清晰度推流（2560×1600@30）
  - `SubScreen-GUI.cmd` — GUI 启动
  - `start-watch.cmd` — 旧"虚拟显示器第三屏"路线的插拔守护（备用）
  - `diagnose-leak.cmd` — 泄漏自测装置（`--no-encode` 二分诊断）

## 鸿蒙端构建

- 前提：DevEco Studio（SDK 6.1.0(23)）。
- 命令行构建：
  ```
  hvigorw.bat assembleHap --mode module -p product=default --no-daemon
  ```
  `DEVECO_SDK_HOME` 必须指向含 `hms` 子目录的 SDK。
- 侧载（需先在 DevEco 里开自动签名）：
  ```
  hdc install -r entry-default-signed.hap
  ```

## 已知问题

- **编码器内存泄漏（known issue）**：Intel QSV 与 Microsoft DX12 编码器栈存在每帧提交内存泄漏（QSV ≈4.7MB/帧，MS DX12 ≈1.7MB/帧），销毁编码器不回收。程序用两层自愈兜底：会话自动重建 + 提交内存超 28GB 进程级重启（几秒画面中断）。默认首选 MS DX12（泄漏最慢），`--encoder Intel` 可切换。
- **NVIDIA 编码器**在混合输出（Optimus）机型上激活失败，已知问题，未解决。

## 排查入口

- `sender.log`：全部日志。
- `crash_probe.log`：心跳 / 异常黑匣子。
- `--no-net` / `--no-encode` / `--sysmem` 三个自测开关可在无平板 / 无网络环境下复现问题。

## License

MIT