// subscreen_sender.cpp — Windows 副屏发送端
// DXGI 桌面捕获(含鼠标合成) → H.264 硬件编码(MF MFT, 优先 NVENC) → TCP 推流(经 hdc fport)
//
// 协议与鸿蒙 SubScreen App 对齐:
//   连接后首包: {"type":"hello","width":W,"height":H}
//   之后每包:   [4字节大端长度][H.264 访问单元(Annex-B, IDR 前保证带 SPS/PPS)]
//
// 用法:
//   subscreen_sender.exe                                  ← 无参数: 打开可视化窗口(推荐)
//   subscreen_sender.exe --list
//   subscreen_sender.exe --source NE160QDM --size 1920x1200 --fps 45   ← 命令行走镜像
//
// 模式:
//   (无参数) / --gui   可视化窗口: 一键"检查平板 → 内屏接入桌面(双屏扩展) → 推流",
//                     再点一下停流并恢复"仅显示屏"。窗口里能看到实时日志。
//   --topology <子命令> 精确开关某块显示器(不改别的屏, 也不需要管理员):
//                     list          列出显示配置里所有可用输出(含未接入的)
//                     on <关键字>    把匹配的显示器接入桌面(双屏扩展)
//                     off <关键字>   把它移出桌面(恢复"仅显示屏")
//                     与 DisplaySwitch /extend 的区别: 后者是粗暴整体扩展, 会把机器上
//                     休眠的其他虚拟屏一起唤醒(本机实测多出一块 DISPLAY9); 精确法只动目标。
//   --list            列出桌面输出(含 --source 可用标识)与硬件编码器, 及 VDD 驱动状态
//   --watch           插拔守护模式: 平板插上自动创建"第三块屏"并推流, 拔下自动销毁。
//                     常驻运行(Ctrl+C 退出), 会自动建 fport。隐含 --extend。
//                     注意: --watch 目前走虚拟显示器路径; 镜像模式的守护尚未接入。
//   --extend [WxH]    单次扩展模式: 创建虚拟显示器并捕获它(别名 --vdd)
//   --vdd-probe       诊断: 创建虚拟显示器后连续观察桌面拓扑 + 实测能否建立桌面复制
//
// 捕获哪块屏(镜像模式, 不需要任何虚拟显示驱动):
//   --source <关键字>  默认主显示器。关键字按 设备名 / 适配器名 / 分辨率 / EDID 型号 匹配,
//                     大小写不敏感、取子串。例: --source NE160QDM / --source BOE0CFB /
//                     --source 1600x2560 / --source "\\.\DISPLAY1"
//                     型号比分辨率稳定, 建议用型号(--list 会列出每块屏的可用关键字)。
//
// 画质与延迟(瓶颈是"编码像素数", 即分辨率×帧率):
//   --size WxH         推流尺寸, 任意值(如 1920x1200)。BGRA→NV12 与缩放全部交给 GPU
//                      (D3D11 VideoProcessor), 只回读缩小后的 NV12 —— 延迟与清晰度的最佳旋钮。
//   --quality low|mid|high  low=30fps+1/2降采样+5Mbps  mid=30fps+8Mbps  high=60fps+20Mbps
//   --scale 1|2|4      整数降采样倍数(纯 CPU 块平均)。功能上被 --size 覆盖, 保留作对照
//   --fps N / --bitrate K  单项覆盖, 会盖掉 --quality 里的对应项
//
// 虚拟显示器(扩展模式):
//   --vdd [WxH[@Hz]]  分辨率(默认 946x1440@60 竖屏, 与平板 2800x1840 同比例)
//   --vdd-fixed [WxH] 强制设为该分辨率; 不加则沿用"上次用过的"(含你在 Windows
//                     显示设置里手动调过的值), 见 exe 同目录 subscreen_state.ini
//   --side left|right 摆在主屏的哪一侧(默认 left)
//   --vdd-preset <子命令>  管理驱动的自定义分辨率预设(写 HKLM, 需管理员):
//                     list | clear | remove <槽位> | add WxH[@Hz] | set WxH[@Hz]
//                     驱动内置模式全是 16:9 横向, 竖屏尺寸必须靠预设加进去,
//                     且改完要用 tools\reload-vdd.ps1 重载适配器才生效
//
// 守护模式细节:
//   --watch-interval MS  设备轮询间隔, 默认 1000
//   --unplug-delay MS    拔出去抖延迟, 默认 2000(线材抖一下不会重建屏)
//   --launch-app         检测到设备时尝试拉起平板 App(锁屏时会失败, 属正常)
//
// 推流:
//     --output N        捕获显示器索引(见 --list), 默认主显示器
//     --fps N           帧率, 默认 30
//     --bitrate K       码率 kbps, 默认按分辨率估算
//     --host H          目标地址, 默认 127.0.0.1 (经 hdc fport)
//     --port P          目标端口, 默认 53517
//     --hdc PATH        hdc.exe 路径, 默认自动探测 DevEco 安装目录
//     --nohdc           不执行 fport (已手动转发时)
//     --encoder KEY     编码器名称过滤, 如 nvenc / intel
//     --duration S      运行 S 秒后自动退出(0=一直运行), 用于自动验证(守护模式忽略)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h> // Shell_NotifyIconW(托盘图标)

#include <d3d11.h>
#include <d3d11_1.h> // ID3D11VideoContext1: 精确设置 VideoProcessor 的色彩空间
#include <d3d10.h>   // ID3D10Multithread: 交给 MF 的 D3D 设备必须开多线程保护
#include <dxgi1_2.h>

#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>

#include <wrl/client.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <clocale>
#include <exception>
#include <stdexcept>

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h> // GetProcessMemoryInfo: 统计行显示进程提交内存, 泄漏一眼可见
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Parsec VDD core API (内核 IddCx 虚拟显示驱动的用户态控制接口, 自带 setupapi/cfgmgr32 链接指令)
#include "third_party/parsec-vdd.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib") // EnumDisplaySettingsExW / ChangeDisplaySettingsExW (虚拟显示器模式与摆位)
#pragma comment(lib, "shell32.lib") // Shell_NotifyIconW(托盘图标)
#pragma comment(lib, "gdi32.lib")  // CreateFontW / DeleteObject / GetDeviceCaps (GUI 字体)
#pragma comment(lib, "advapi32.lib") // 注册表(HKLM\SOFTWARE\Parsec\vdd 自定义分辨率预设)
#pragma comment(lib, "psapi.lib")    // GetProcessMemoryInfo(统计行的进程提交内存)

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

static std::atomic<bool> g_exit{false};
// 新编码器(重连)就绪后强制推一帧, 否则桌面静止时接收端收不到 IDR 一直黑屏
static std::atomic<bool> g_forceFrame{false};

// GUI 模式下额外把日志收进内存缓冲, 由窗口定时器贴到日志框里。
// 控制台行为完全不变(命令行/脚本照旧)。
static std::mutex g_logMx;
static std::string g_logBuf;
static std::atomic<bool> g_logCapture{false};
// Windows 子系统下通常没有控制台: 只有被重定向(> log / 管道)时 stdout 句柄才有效。
// 无效时 printf 要跳过(写无效句柄不可靠)。
static bool g_stdoutOK = false;

// 文件日志: 托盘模式下控制窗口不可见, 全部日志同步落 sender.log(超过 4MB 归零)。
static void LogFileWrite(const char *line)
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    size_t s = p.find_last_of("\\/");
    if (s != std::string::npos) {
        p = p.substr(0, s + 1);
    }
    p += "sender.log";
    FILE *f = fopen(p.c_str(), "ab");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) > 4LL * 1024 * 1024) {
        fclose(f);
        f = fopen(p.c_str(), "wb");
        if (!f) {
            return;
        }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
}

static void GuiLogTake(std::string *out)
{
    std::lock_guard<std::mutex> lk(g_logMx);
    *out = std::move(g_logBuf);
    g_logBuf.clear();
}

static void Log(const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_stdoutOK) {
        printf("%s\n", line);
        fflush(stdout);
    }
    LogFileWrite(line);

    if (g_logCapture.load()) {
        std::lock_guard<std::mutex> lk(g_logMx);
        g_logBuf += line;
        g_logBuf += "\r\n";
        if (g_logBuf.size() > 262144) { // 防止无限增长
            g_logBuf.erase(0, g_logBuf.size() - 196608);
        }
    }
}

// ============================ 崩溃取证 ============================
//
// 0xc0000409(fastfail) 不走 UnhandledExceptionFilter, 但分两种来历:
//   a) 未捕获 C++ 异常 → std::terminate → abort → fastfail   ← set_terminate 能抓到
//   b) /GS 栈保护 (__report_gsfailure)                        ← 抓不到, 只能靠事件日志偏移+map 定位
// 无论哪种, 事件日志里的"错误偏移"+ 编译出的 .map 都能反查出函数名。
// 心跳与阶段标记写入 crash_probe.log, 崩溃后能对出最后活着的状态。

static void ProbeLog(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    size_t s = p.find_last_of("\\/");
    if (s != std::string::npos) {
        p = p.substr(0, s + 1);
    }
    p += "crash_probe.log";
    FILE *f = fopen(p.c_str(), "ab");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03u][tid %lu] %s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, GetCurrentThreadId(), line);
        fclose(f);
    }
}

static void TerminateProbe()
{
    ProbeLog("std::terminate called!");
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            ProbeLog("  uncaught exception: %s", e.what());
        } catch (...) {
            ProbeLog("  uncaught non-std exception");
        }
    }
    std::abort(); // 走默认的 fastfail 收场
}

// bad_alloc 时抓系统/进程内存快照: 区分"真耗尽"(各可用量≈0)和"堆损坏"(明明还有大量内存却分配失败)。
static void ProbeMem(const char *where)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    ProbeLog("bad_alloc in %s: memLoad=%u%% physFree=%lluMB pageFree=%lluMB virtualFree=%lluMB",
             where, ms.dwMemoryLoad, ms.ullAvailPhys / 1048576, ms.ullAvailPageFile / 1048576,
             ms.ullAvailVirtual / 1048576);
}

// 本进程提交内存(共享给统计行与探针): PagefileUsage ≈ commit charge。
static uint64_t ProbeCommitMB()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PagefileUsage / 1048576;
}

// ⭐ 进程级自愈: Intel QSV MFT 存在每帧 ~4.7MB 的提交内存泄漏, 且泄漏块**不随编码器
// 销毁而释放**(实测 session restart 前后 commit 一字不差), 唯一回收方式是进程退出。
// 所以: 水位超阈值 → 启动新的自己(--gui-autostart 自动继续推流) → 释放单实例互斥 →
// 退出本进程(系统回收全部泄漏)。画面中断约 3~5 秒。
static HANDLE g_guiMutex = nullptr; // 单实例互斥(RunGui 创建), 自重启前要先放手

static void RestartProcess(const char *reason)
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --gui-autostart";
    Log("[自愈] %s —— 提交 %lluMB, 自动重启进程续命(画面将中断几秒)", reason, ProbeCommitMB());
    ProbeLog("auto-restart: %s commit=%lluMB", reason, ProbeCommitMB());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log("[自愈] 重启失败: 0x%08lX (请手动重新打开)", GetLastError());
        ProbeLog("auto-restart FAILED 0x%08lX", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // 先放手单实例互斥, 新进程才能通过检查; 然后整体退出, 系统回收全部泄漏
    if (g_guiMutex) {
        ReleaseMutex(g_guiMutex);
        CloseHandle(g_guiMutex);
        g_guiMutex = nullptr;
    }
    ExitProcess(0);
}

// ============================ 帧队列(有界, 满丢最旧) ============================

struct FrameBuf
{
    std::vector<uint8_t> nv12;
    LONGLONG ts = 0;
    LONGLONG dur = 0;
    // 这一帧被捕获的时刻。一路带着走, 用于统计"捕获 → 编码出 AU"的端到端延迟,
    // 把"调优靠感觉"变成"调优看数字"。
    std::chrono::steady_clock::time_point capT{};
};

class FrameQueue
{
public:
    void Push(FrameBuf &&f)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            // 深度 1: 编码器忙不过来时直接丢掉上一帧, 永远只处理最新的一帧。
            // 深度越大越"平滑", 但每一帧的排队时间都会变成用户感知到的延迟 —— 投屏要的是跟手,
            // 所以宁可丢帧也不攒帧。
            if (q_.size() >= 1) {
                q_.pop();
                dropped_++;
            }
            q_.push(std::move(f));
        }
        cv_.notify_all();
    }

    bool Pop(FrameBuf &out)
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return closed_ || g_exit.load() || !q_.empty(); });
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void Close()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    uint64_t Dropped() const { return dropped_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<FrameBuf> q_;
    bool closed_ = false;
    uint64_t dropped_ = 0;
};

// 捕获线程 → 当前编码器的帧通道(编码器按连接重建, 需要防悬空指针)
static std::mutex g_feedMtx;
static FrameQueue *g_feedQueue = nullptr;

static void FeedGlobal(FrameBuf &&f)
{
    std::lock_guard<std::mutex> lk(g_feedMtx);
    if (g_feedQueue != nullptr) {
        g_feedQueue->Push(std::move(f));
    }
}

static void SetFeedTarget(FrameQueue *q)
{
    std::lock_guard<std::mutex> lk(g_feedMtx);
    g_feedQueue = q;
}

// ============================ BGRA → NV12 (BT.601 limited, SSE) ============================

static void BgraToNv12(const uint8_t *bgra, size_t pitch, uint8_t *nv12, uint8_t *uPlane, uint8_t *vPlane,
                       int w, int h)
{
    const __m128i maskR = _mm_setr_epi8(2, 6, 10, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i maskG = _mm_setr_epi8(1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i maskB = _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i maskEven =
        _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 0, 2, 4, 6, 8, 10, 12, 14);

    for (int y = 0; y < h; y++) {
        const uint8_t *src = bgra + y * pitch;
        uint8_t *Y = nv12 + (size_t)y * w;
        uint8_t *U = uPlane + (size_t)y * w;
        uint8_t *V = vPlane + (size_t)y * w;
        int x = 0;
        for (; x + 4 <= w; x += 4) {
            __m128i px = _mm_loadu_si128((const __m128i *)(src + (size_t)x * 4));
            __m128i r = _mm_cvtepu8_epi32(_mm_shuffle_epi8(px, maskR));
            __m128i g = _mm_cvtepu8_epi32(_mm_shuffle_epi8(px, maskG));
            __m128i b = _mm_cvtepu8_epi32(_mm_shuffle_epi8(px, maskB));

            // Y = ((66R + 129G + 25B + 128) >> 8) + 16
            // 注意: 必须用**算术**右移(_mm_srai_epi32)。这几个乘加式经常为负,
            // 逻辑右移(_mm_srli_epi32)会把负数变成巨大正数, 再经 packusdw 饱和成 65535,
            // 而 65535 当有符号 16 位看就是 -1, 最后 packuswb 会把它钳成 **0** ——
            // 结果就是色度平面被清零, 画面整体泛绿(实测踩过: U/V 有 48.7% 字节变成 0)。
            // 标量尾巴用的是 C 的 >>(算术), 所以只有 SSE 覆盖的像素会坏。
            __m128i t = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(r, _mm_set1_epi32(66)),
                                                    _mm_mullo_epi32(g, _mm_set1_epi32(129))),
                                      _mm_add_epi32(_mm_mullo_epi32(b, _mm_set1_epi32(25)),
                                                    _mm_set1_epi32(128)));
            __m128i yy = _mm_add_epi32(_mm_srai_epi32(t, 8), _mm_set1_epi32(16));
            __m128i y16 = _mm_packus_epi32(yy, yy);
            *(uint32_t *)(Y + x) = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(y16, y16));

            // U = ((-38R - 74G + 112B + 128) >> 8) + 128   ← 必须算术右移(见上方说明)
            __m128i uu = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(
                                           _mm_add_epi32(_mm_mullo_epi32(r, _mm_set1_epi32(-38)),
                                                         _mm_mullo_epi32(g, _mm_set1_epi32(-74))),
                                           _mm_add_epi32(_mm_mullo_epi32(b, _mm_set1_epi32(112)),
                                                         _mm_set1_epi32(128))),
                                       8),
                                       _mm_set1_epi32(128));
            __m128i u16 = _mm_packus_epi32(uu, uu);
            *(uint32_t *)(U + x) = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(u16, u16));

            // V = ((112R - 94G - 18B + 128) >> 8) + 128   ← 必须算术右移(见上方说明)
            __m128i vv = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(
                                           _mm_add_epi32(_mm_mullo_epi32(r, _mm_set1_epi32(112)),
                                                         _mm_mullo_epi32(g, _mm_set1_epi32(-94))),
                                           _mm_add_epi32(_mm_mullo_epi32(b, _mm_set1_epi32(-18)),
                                                         _mm_set1_epi32(128))),
                                       8),
                                       _mm_set1_epi32(128));
            __m128i v16 = _mm_packus_epi32(vv, vv);
            *(uint32_t *)(V + x) = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(v16, v16));
        }
        for (; x < w; x++) {
            const uint8_t *p = src + (size_t)x * 4;
            int R = p[2], G = p[1], B = p[0];
            Y[x] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
            U[x] = (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            V[x] = (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }

    // 2x2 平均 → NV12 UV 交织平面
    uint8_t *uv = nv12 + (size_t)w * h;
    for (int y = 0; y + 1 < h; y += 2) {
        const uint8_t *U0 = uPlane + (size_t)y * w;
        const uint8_t *U1 = U0 + w;
        const uint8_t *V0 = vPlane + (size_t)y * w;
        const uint8_t *V1 = V0 + w;
        uint8_t *dst = uv + (size_t)(y / 2) * w;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m128i ua = _mm_avg_epu8(_mm_loadu_si128((const __m128i *)(U0 + x)),
                                      _mm_loadu_si128((const __m128i *)(U1 + x)));
            __m128i va = _mm_avg_epu8(_mm_loadu_si128((const __m128i *)(V0 + x)),
                                      _mm_loadu_si128((const __m128i *)(V1 + x)));
            __m128i uh = _mm_avg_epu8(ua, _mm_srli_si128(ua, 1));
            __m128i vh = _mm_avg_epu8(va, _mm_srli_si128(va, 1));
            __m128i uEven = _mm_shuffle_epi8(uh, maskEven);
            __m128i vEven = _mm_shuffle_epi8(vh, maskEven);
            _mm_storeu_si128((__m128i *)(dst + x), _mm_unpacklo_epi8(uEven, vEven));
        }
        for (; x + 1 < w; x += 2) {
            int u = (U0[x] + U0[x + 1] + U1[x] + U1[x + 1] + 2) >> 2;
            int v = (V0[x] + V0[x + 1] + V1[x] + V1[x + 1] + 2) >> 2;
            dst[x] = (uint8_t)u;
            dst[x + 1] = (uint8_t)v;
        }
        if (x < w) { // 宽度为奇数(显示器实际不会出现)
            dst[x] = (uint8_t)((U0[x] + U1[x] + 1) >> 1);
            dst[x + 1] = (uint8_t)((V0[x] + V1[x] + 1) >> 1);
        }
    }
    if (h % 2 == 1) { // 高度为奇数兜底: 复制上一行
        memcpy(uv + (size_t)(h / 2) * w, uv + (size_t)(h / 2 - 1) * w, (size_t)w);
    }
}

// ============================ NV12 降采样 (+ 备用旋转) ============================

// 【当前不调用, 保留备用】
// 用户明确要求"不要任何旋转逻辑": 画面纯映射, 显示方向由用户自己在 Windows 设置里调。
// 所以 90/270 旋转已从捕获流程里移除。这个函数留着是因为它验证过、可复用:
// 若哪天需要把竖屏面板的原始帧转成桌面方向, 直接调用即可(方向是顺时针/逆时针由 cw 决定)。
//
// 把 BGRA 画面旋转 90 度。用"块转置"实现: 读写两侧都保持连续访问, 避免逐像素跨行
// stride 造成的缓存击穿(直接按公式走会变成每写一个像素就 miss 一条 cache line)。
//
// 逆时针/顺时针的映射(见 DXGI_OUTPUT_DESC.Rotation 的语义):
//   顺时针 ROTATE90 :  dst(x, y) = src(sx = y,           sy = sh - 1 - x)
//   逆时针 ROTATE270:  dst(x, y) = src(sx = sw - 1 - y,  sy = x)
// src 尺寸 sw×sh, dst 尺寸 sh×sw。
static void RotateBgra90(const uint32_t *src, int sw, int sh, uint32_t *dst, bool cw)
{
    const int dw = sh; // dst 宽
    const int dh = sw; // dst 高
    const int T = 16;
    uint32_t blk[16][16];
    for (int y0 = 0; y0 < dh; y0 += T) {
        const int ye = (y0 + T < dh) ? y0 + T : dh;
        for (int x0 = 0; x0 < dw; x0 += T) {
            const int xe = (x0 + T < dw) ? x0 + T : dw;
            const int tw = xe - x0; // 本块列数(= x 方向)
            const int th = ye - y0; // 本块行数(= y 方向)
            if (cw) {
                for (int i = 0; i < th; i++) {
                    const uint32_t *r = src + (size_t)(sh - 1 - (x0 + i)) * sw;
                    for (int j = 0; j < tw; j++) {
                        blk[i][j] = r[y0 + j];
                    }
                }
            } else {
                for (int i = 0; i < th; i++) {
                    const uint32_t *r = src + (size_t)(x0 + i) * sw;
                    for (int j = 0; j < tw; j++) {
                        blk[i][j] = r[sw - 1 - (y0 + j)];
                    }
                }
            }
            for (int j = 0; j < tw; j++) {
                uint32_t *d = dst + (size_t)(y0 + j) * dw;
                for (int i = 0; i < th; i++) {
                    d[x0 + i] = blk[i][j];
                }
            }
        }
    }
}

// NV12 整数倍降采样(块平均)。目的: 在不改用户桌面分辨率的前提下, 降低编码/解码负担,
// 让平板端更流畅。k 必须整除 sw/sh, 且 dw=sw/k、dh=sh/k 为偶数(UV 按 2x2 取样)。
static void Nv12Downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int k)
{
    const int dw = sw / k;
    const int dh = sh / k;
    const unsigned n = (unsigned)(k * k);

    for (int y = 0; y < dh; y++) {
        const uint8_t *row0 = src + (size_t)(y * k) * sw;
        uint8_t *d = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            unsigned sum = 0;
            for (int dy = 0; dy < k; dy++) {
                const uint8_t *r = row0 + (size_t)dy * sw + (size_t)x * k;
                for (int dx = 0; dx < k; dx++) {
                    sum += r[dx];
                }
            }
            d[x] = (uint8_t)(sum / n);
        }
    }

    const uint8_t *suv = src + (size_t)sw * sh;
    uint8_t *duv = dst + (size_t)dw * dh;
    for (int y = 0; y < dh / 2; y++) {
        uint8_t *drow = duv + (size_t)y * dw;
        for (int x = 0; x < dw / 2; x++) {
            unsigned su = 0, sv = 0;
            for (int dy = 0; dy < k; dy++) {
                const uint8_t *r = suv + (size_t)(y * k + dy) * sw + (size_t)x * k * 2;
                for (int dx = 0; dx < k; dx++) {
                    su += r[dx * 2];
                    sv += r[dx * 2 + 1];
                }
            }
            drow[x * 2] = (uint8_t)(su / n);
            drow[x * 2 + 1] = (uint8_t)(sv / n);
        }
    }
}

// ============================ 通用 NV12 缩放(双线性, 任意尺寸) ============================
//
// 只在两种情况下用到: ① GPU 路径不可用而用户指定了 --size; ② 该帧需要合成鼠标光标。
// 常规帧走 GpuScaler, 不经过这里。

static void Nv12ScaleBilinear(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    auto axis = [](int i, int n, int sn, int *i0, int *i1, float *t) {
        float f = ((float)i + 0.5f) * (float)sn / (float)n - 0.5f;
        int a = (int)floorf(f);
        *t = f - (float)a;
        if (a < 0) {
            a = 0;
            *t = 0;
        }
        if (a > sn - 1) {
            a = sn - 1;
            *t = 0;
        }
        int b = a + 1;
        if (b > sn - 1) {
            b = sn - 1;
        }
        *i0 = a;
        *i1 = b;
    };

    for (int y = 0; y < dh; y++) {
        int y0, y1;
        float ty;
        axis(y, dh, sh, &y0, &y1, &ty);
        const uint8_t *r0 = src + (size_t)y0 * sw;
        const uint8_t *r1 = src + (size_t)y1 * sw;
        uint8_t *d = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int x0, x1;
            float tx;
            axis(x, dw, sw, &x0, &x1, &tx);
            float v = (r0[x0] * (1 - tx) + r0[x1] * tx) * (1 - ty) +
                      (r1[x0] * (1 - tx) + r1[x1] * tx) * ty;
            d[x] = (uint8_t)(v + 0.5f);
        }
    }

    const uint8_t *suv = src + (size_t)sw * sh;
    uint8_t *duv = dst + (size_t)dw * dh;
    const int csw = sw / 2, csh = sh / 2, cdw = dw / 2, cdh = dh / 2;
    for (int y = 0; y < cdh; y++) {
        int y0, y1;
        float ty;
        axis(y, cdh, csh, &y0, &y1, &ty);
        const uint8_t *r0 = suv + (size_t)y0 * sw;
        const uint8_t *r1 = suv + (size_t)y1 * sw;
        uint8_t *d = duv + (size_t)y * dw;
        for (int x = 0; x < cdw; x++) {
            int x0, x1;
            float tx;
            axis(x, cdw, csw, &x0, &x1, &tx);
            for (int c = 0; c < 2; c++) {
                float v = (r0[x0 * 2 + c] * (1 - tx) + r0[x1 * 2 + c] * tx) * (1 - ty) +
                          (r1[x0 * 2 + c] * (1 - tx) + r1[x1 * 2 + c] * tx) * ty;
                d[x * 2 + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

// ============================ GPU 色彩转换 + 缩放 (D3D11 VideoProcessor) ============================
//
// 把桌面 BGRA 帧在 GPU 上一步转成 NV12 并缩放到推流尺寸, CPU 只回读缩小后的 NV12。
// 相比纯 CPU 路径省掉三件事(均为实测):
//   - 整幅 BGRA 回读: 2560x1600 BGRA = 16MB  →  1920x1200 NV12 = 3.4MB
//   - BGRA→NV12 转换: 实测 6.7ms
//   - 块平均降采样:   实测数 ms
// 缩放由 GPU 线性滤波完成: 质量明显优于整数倍块平均, 且尺寸任意(不再受 1/2/4 限制),
// 于是可以选 1920x1200 这类"中间档" —— 既比 1280x800 清楚得多, 又比 2560x1600 省编码时间。
//
// 色彩空间: 输入按 RGB full(桌面本身就是 full range), 输出按 BT.601 studio(limited),
// 与 CPU 路径(BgraToNv12 用 BT.601 limited 系数)严格一致 —— 否则平板端颜色会整体偏掉。

class GpuScaler
{
public:
    bool Init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int srcW, int srcH, int dstW, int dstH,
              std::string *err)
    {
        dev_ = dev;
        ctx_ = ctx;
        dstW_ = dstW;
        dstH_ = dstH;

        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_)))) {
            *err = "ID3D11VideoDevice 不可用";
            return false;
        }
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_)))) {
            *err = "ID3D11VideoContext 不可用";
            return false;
        }
        // 经典色彩空间 API 的语义很容易搞反(full/limited 与 601/709 的组合), 用 Context1 更稳。
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx1_)))) {
            *err = "ID3D11VideoContext1 不可用(需 Win10)";
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = (UINT)srcW;
        cd.InputHeight = (UINT)srcH;
        cd.OutputWidth = (UINT)dstW;
        cd.OutputHeight = (UINT)dstH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
        if (FAILED(hr)) {
            *err = "CreateVideoProcessorEnumerator 失败 0x" + Hex(hr);
            return false;
        }

        // 必须确认驱动真的支持这条格式通路, 否则 Blt 会静默失败或输出花屏
        UINT sup = 0;
        if (FAILED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &sup)) ||
            !(sup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
            *err = "驱动不支持 BGRA 作为 VideoProcessor 输入";
            return false;
        }
        sup = 0;
        if (FAILED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &sup)) ||
            !(sup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            *err = "驱动不支持 NV12 作为 VideoProcessor 输出";
            return false;
        }

        hr = vdev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
        if (FAILED(hr)) {
            *err = "CreateVideoProcessor 失败 0x" + Hex(hr);
            return false;
        }

        // 输出 NV12 纹理(VideoProcessorBlt 的目标)
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)dstW;
        td.Height = (UINT)dstH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = dev->CreateTexture2D(&td, nullptr, &tex_);
        if (FAILED(hr)) {
            *err = "NV12 目标纹理创建失败 0x" + Hex(hr);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        hr = vdev_->CreateVideoProcessorOutputView(tex_.Get(), vpEnum_.Get(), &ovd, &outView_);
        if (FAILED(hr)) {
            *err = "CreateVideoProcessorOutputView 失败 0x" + Hex(hr);
            return false;
        }

        // 回读用的 staging
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateTexture2D(&td, nullptr, &stage_);
        if (FAILED(hr)) {
            *err = "NV12 回读纹理创建失败 0x" + Hex(hr);
            return false;
        }

        // 色彩空间: BGRA full-range sRGB → NV12 BT.601 studio(limited)
        vctx1_->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1_->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
        // 线性滤波: 缩放质量(默认可能是 nearest, 那样缩下来的小图会很难看)
        // 注意: D3D11 没给"缩放滤波质量"提供开关(那套 FILTER_* 是亮度/对比度等图像增强,
        // 与缩放无关), 缩放滤波由驱动自行选择, 实测效果可用。

        ready_ = true;
        return true;
    }

    // 一帧转换。dstNv12 必须已按 dstW*dstH*3/2 分配。
    bool Convert(ID3D11Texture2D *srcTex, uint8_t *dstNv12)
    {
        if (!ready_) {
            return false;
        }
        // 输入视图与源纹理绑定, 换纹理才重建(重建 dup 后纹理会变, 故 ResetInputView 要配套调用)
        if (inTex_ != srcTex) {
            inView_.Reset();
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.FourCC = 0;
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivd.Texture2D.MipSlice = 0;
            ivd.Texture2D.ArraySlice = 0;
            if (FAILED(vdev_->CreateVideoProcessorInputView(srcTex, vpEnum_.Get(), &ivd, &inView_))) {
                return false;
            }
            inTex_ = srcTex;
        }

        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = inView_.Get();
        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), outView_.Get(), 0, 1, &st))) {
            return false;
        }

        ctx_->CopyResource(stage_.Get(), tex_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(stage_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            return false;
        }
        // NV12: 前 dstH 行是 Y, 紧接着 dstH/2 行是交错 UV(每行仍以 RowPitch 为步长)
        const uint8_t *sy = (const uint8_t *)m.pData;
        const uint8_t *suv = sy + (size_t)m.RowPitch * dstH_;
        for (int y = 0; y < dstH_; y++) {
            memcpy(dstNv12 + (size_t)y * dstW_, sy + (size_t)y * m.RowPitch, (size_t)dstW_);
        }
        uint8_t *duv = dstNv12 + (size_t)dstW_ * dstH_;
        for (int y = 0; y < dstH_ / 2; y++) {
            memcpy(duv + (size_t)y * dstW_, suv + (size_t)y * m.RowPitch, (size_t)dstW_);
        }
        ctx_->Unmap(stage_.Get(), 0);
        return true;
    }

    void ResetInputView()
    {
        inView_.Reset();
        inTex_ = nullptr;
    }

    bool Ready() const { return ready_; }
    int DstW() const { return dstW_; }
    int DstH() const { return dstH_; }

private:
    static std::string Hex(long hr)
    {
        char b[16];
        snprintf(b, sizeof(b), "%08lX", (unsigned long)hr);
        return b;
    }

    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoContext1> vctx1_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11Texture2D> tex_;   // NV12 输出
    ComPtr<ID3D11Texture2D> stage_; // NV12 回读
    ComPtr<ID3D11VideoProcessorOutputView> outView_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    ID3D11Texture2D *inTex_ = nullptr;
    int dstW_ = 0, dstH_ = 0;
    bool ready_ = false;
};

// ============================ H.264 AU 修正(保证 IDR 前有 SPS/PPS) ============================

class AuFixer
{
public:
    void Process(const uint8_t *au, size_t len, std::vector<uint8_t> &out)
    {
        out.clear();
        bool hasIdr = false;
        bool hasSps = false;
        size_t pos = 0;
        while (pos + 4 <= len) {
            size_t start = 0;
            size_t sc = 0;
            if (au[pos] == 0 && au[pos + 1] == 0 && au[pos + 2] == 1) {
                start = pos;
                sc = 3;
            } else if (au[pos] == 0 && au[pos + 1] == 0 && au[pos + 2] == 0 && au[pos + 3] == 1) {
                start = pos;
                sc = 4;
            } else {
                pos++;
                continue;
            }
            size_t next = pos + 1;
            while (next + 3 <= len) {
                if (au[next] == 0 && au[next + 1] == 0 &&
                    (au[next + 2] == 1 || (au[next + 2] == 0 && next + 4 <= len && au[next + 3] == 1))) {
                    break;
                }
                next++;
            }
            size_t nalStart = start + sc;
            size_t nalEnd = (next + 3 <= len) ? next : len;
            if (nalEnd > nalStart) {
                uint8_t t = au[nalStart] & 0x1F;
                if (t == 7) {
                    hasSps = true;
                    sps_.assign(au + start, au + nalEnd);
                } else if (t == 8) {
                    pps_.assign(au + start, au + nalEnd);
                } else if (t == 5) {
                    hasIdr = true;
                }
            }
            pos = next;
        }
        if (hasIdr && !hasSps && !sps_.empty() && !pps_.empty()) {
            out.reserve(sps_.size() + pps_.size() + len);
            out.insert(out.end(), sps_.begin(), sps_.end());
            out.insert(out.end(), pps_.begin(), pps_.end());
            out.insert(out.end(), au, au + len);
            return;
        }
        out.assign(au, au + len);
    }

private:
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
};

// ============================ DXGI 桌面捕获(含鼠标) ============================

// ============================ 显示器识别(供 --source 稳定匹配) ============================
//
// 为什么需要它: 挑"笔记本内屏"这类目标时, 两种直觉做法都不可靠 ——
//   * \\.\DISPLAYn 会随插拔/黑屏重新编号;
//   * 分辨率可能被用户改掉。
// 真正稳定的只有显示器 EDID 里的厂商/产品码(内屏是 BOE0CFB, 型号名 NE160QDM-NZC)。
// 这里把 \\.\DISPLAYn 映射到 "monitorDevicePath|friendlyName", 由 CCD(显示配置 API)取得。
// 注意: CcdTargetId 的实现在后面虚拟显示器那一节, 这里先声明。

static std::wstring CcdTargetId(const DISPLAYCONFIG_PATH_INFO &p);

static std::wstring Utf8ToWide(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    }
    return w;
}

static bool WStrContainsNoCase(const std::wstring &hay, const std::wstring &needle)
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        for (; j < needle.size(); j++) {
            if (towlower(hay[i + j]) != towlower(needle[j])) {
                break;
            }
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

// 当前活动的 \\.\DISPLAYn → 型号识别串
static std::vector<std::pair<std::wstring, std::wstring>> CcdActiveDisplayNames()
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPath, &nMode) != ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode ? nMode : 1);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return out;
    }
    paths.resize(nPath);
    for (auto &p : paths) {
        if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring id = CcdTargetId(p);
        if (!id.empty()) {
            out.push_back(std::make_pair(std::wstring(sn.viewGdiDeviceName), id));
        }
    }
    return out;
}

// 供 --source 的提示: 把每个输出可用的匹配关键字打出来(实现在 EnumOutputs 之后)

struct OutputEntry
{
    int index = 0;
    std::wstring adapterName;
    std::wstring deviceName;
    DXGI_OUTPUT_DESC desc{};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
};

// attachedOnly=true 只返回已挂到桌面的输出(可捕获); false 时把枚举到的全部输出都列出来,
// 用于诊断"显示器被驱动创建了但没挂上桌面"这一类问题。
static std::vector<OutputEntry> EnumOutputs(bool attachedOnly = true)
{
    std::vector<OutputEntry> list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return list;
    }
    int idx = 0;
    for (UINT a = 0;; a++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            OutputEntry e;
            e.index = idx++;
            e.adapterName = ad.Description;
            out->GetDesc(&e.desc);
            e.deviceName = e.desc.DeviceName;
            e.adapter = adapter;
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(out.As(&out1))) {
                e.output = out1;
            }
            if (!attachedOnly || e.desc.AttachedToDesktop) {
                list.push_back(e);
            }
        }
    }
    return list;
}

// 打印全部输出(含未挂桌面的), 用于诊断
static void DumpOutputs(const char *tag)
{
    auto all = EnumOutputs(false);
    int attached = 0;
    for (auto &e : all) {
        if (e.desc.AttachedToDesktop) {
            attached++;
        }
    }
    Log("[%s] 枚举 %zu 个输出, 其中挂到桌面的 %d 个", tag, all.size(), attached);
    for (auto &e : all) {
        Log("      #%d %ls [%ls] %d×%d @(%ld,%ld) rot=%d %s", e.index, e.deviceName.c_str(),
            e.adapterName.c_str(), e.desc.DesktopCoordinates.right - e.desc.DesktopCoordinates.left,
            e.desc.DesktopCoordinates.bottom - e.desc.DesktopCoordinates.top, e.desc.DesktopCoordinates.left,
            e.desc.DesktopCoordinates.top, (int)e.desc.Rotation,
            e.desc.AttachedToDesktop ? "已挂桌面" : "**未挂桌面**");
    }
}

// 打印 DXGI 适配器层级(含输出个数), 用于判断虚拟显示适配器本身是否还活着
static void DumpAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Log("[适配器] 创建 DXGI Factory 失败");
        return;
    }
    Log("[适配器] DXGI 适配器列表:");
    for (UINT a = 0;; a++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        int outs = 0;
        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            outs++;
        }
        Log("      #%u %ls (输出 %d 个)", a, ad.Description, outs);
    }
}

// 打印每个输出可用的 --source 匹配关键字(设备名 / 分辨率 / EDID 型号)
static void ListSourceKeys()
{
    auto nameMap = CcdActiveDisplayNames();
    Log("       可用 --source 关键字:");
    for (auto &e : EnumOutputs()) {
        int w = e.desc.DesktopCoordinates.right - e.desc.DesktopCoordinates.left;
        int h = e.desc.DesktopCoordinates.bottom - e.desc.DesktopCoordinates.top;
        std::wstring model;
        for (auto &kv : nameMap) {
            if (kv.first == e.deviceName) {
                model = kv.second;
                break;
            }
        }
        Log("         #%d  %ls  %dx%d  %ls", e.index, e.deviceName.c_str(), w, h,
            model.empty() ? L"(无型号信息)" : model.c_str());
    }
}

class DupCapture
{
public:
    // scale: 整数降采样倍数(1/2/4), 仅在不指定 --size 时生效
    // wantW/wantH: --size 指定的推流尺寸(任意值, 走 GPU 缩放), 0 表示不用
    bool Init(int outputIndex, const std::string &sourceMatch, int scale, int wantW, int wantH, int *outW,
              int *outH)
    {
        auto outputs = EnumOutputs();
        if (outputs.empty()) {
            Log("[错误] 未找到任何连接到桌面的显示器输出");
            return false;
        }
        const OutputEntry *sel = nullptr;
        if (!sourceMatch.empty()) {
            // --source: 依次比对 设备名(\\.\DISPLAYn) / 适配器名 / 分辨率(WxH) / EDID 型号。
            // 期望用法: --source 1600x2560 或 --source BOE0CFB 或 --source NE160QDM
            std::wstring want = Utf8ToWide(sourceMatch);
            auto nameMap = CcdActiveDisplayNames();
            for (auto &e : outputs) {
                int ew = e.desc.DesktopCoordinates.right - e.desc.DesktopCoordinates.left;
                int eh = e.desc.DesktopCoordinates.bottom - e.desc.DesktopCoordinates.top;
                std::wstring hay = e.deviceName + L" " + e.adapterName + L" " + std::to_wstring(ew) + L"x" +
                                   std::to_wstring(eh);
                for (auto &kv : nameMap) {
                    if (kv.first == e.deviceName) {
                        hay += L" " + kv.second;
                        break;
                    }
                }
                if (WStrContainsNoCase(hay, want)) {
                    sel = &e;
                    break;
                }
            }
            if (!sel) {
                Log("[错误] --source \"%s\" 没有匹配到任何已连接的输出", sourceMatch.c_str());
                ListSourceKeys();
                return false;
            }
            Log("--source \"%s\" 命中 %ls", sourceMatch.c_str(), sel->deviceName.c_str());
        } else if (outputIndex >= 0) {
            for (auto &e : outputs) {
                if (e.index == outputIndex) {
                    sel = &e;
                    break;
                }
            }
            if (!sel) {
                Log("[错误] --output %d 不存在 (共 %zu 个输出)", outputIndex, outputs.size());
                return false;
            }
        } else {
            for (auto &e : outputs) {
                if (e.desc.DesktopCoordinates.left == 0 && e.desc.DesktopCoordinates.top == 0) {
                    sel = &e;
                    break;
                }
            }
            if (!sel) {
                sel = &outputs[0];
            }
        }
        outRect_ = sel->desc.DesktopCoordinates;
        w_ = outRect_.right - outRect_.left;
        h_ = outRect_.bottom - outRect_.top;
        if (sel->output == nullptr) {
            Log("[错误] 输出 %ls 不支持 IDXGIOutput1, 无法桌面复制", sel->deviceName.c_str());
            return false;
        }
        // ===== 纯映射模式(用户要求: 不要任何旋转逻辑) =====
        // 不做任何像素旋转, 抓到什么就送什么。
        // 这里只做一件事: 把画布尺寸对到 DDA **实际交出的帧尺寸**。
        // 实测(2026-09-14) DDA 交出来的是"面板原始方向": 内屏桌面方向 1600x2560,
        // 实际帧 2560x1600。尺寸对不上整帧会被静默丢弃(表现为死循环重建), 所以 90/270
        // 时交换一下宽高 —— 这只是把画布尺寸对齐, 不动任何像素。
        // 用户把内屏方向调成"横向"(0°)之后, 桌面尺寸与实际帧尺寸一致, 这里完全等价于直通。
        if (sel->desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            sel->desc.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
            int t = w_;
            w_ = h_;
            h_ = t;
            Log("[纯映射] 显示器当前旋转 90/270, 画布按实际帧尺寸 %dx%d 处理(不旋转像素)", w_, h_);
            Log("        把内屏方向调成\"横向\"后画面方向自然就正了(改完需重启本程序)");
        }
        srcW_ = w_;
        srcH_ = h_;
        outRect_.right = outRect_.left + w_;
        outRect_.bottom = outRect_.top + h_;

        HRESULT hr = D3D11CreateDevice(sel->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (FAILED(hr)) {
            Log("[错误] D3D11 设备创建失败: 0x%08lX", hr);
            return false;
        }
        Log("[dbg] D3D11 OK, dup...");
        output_ = sel->output; // 从局部枚举结果拷贝 COM 引用
        if (!RecreateDup()) {
            return false;
        }
        Log("[dbg] dup OK, staging...");

        // 诊断: DDA 声明的尺寸常与"实际帧尺寸"不一致(本机内屏就是这种), 真伪以 Tick 为准。
        {
            DXGI_OUTDUPL_DESC dd{};
            dup_->GetDesc(&dd);
            Log("[dbg] DDA 声明 %ux%u | 实际采用 %dx%d | Rotation=%d", dd.ModeDesc.Width,
                dd.ModeDesc.Height, srcW_, srcH_, (int)sel->desc.Rotation);
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)srcW_; // 用实际帧尺寸建 staging, 否则尺寸不符会被整帧丢弃
        td.Height = (UINT)srcH_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev_->CreateTexture2D(&td, nullptr, &staging_);
        if (FAILED(hr)) {
            Log("[错误] staging 纹理创建失败: 0x%08lX", hr);
            return false;
        }

        // ===== 推流尺寸决策: --size(GPU 缩放, 任意尺寸) > --scale(整数降采样) > 原尺寸 =====
        scale_ = 1;
        ow_ = w_;
        oh_ = h_;
        std::string how = "原尺寸";

        if (wantW > 0 && wantH > 0) {
            // --size: 任意尺寸, 交给 GPU 缩放。NV12 要求宽高为偶数。
            int tw = wantW & ~1;
            int th = wantH & ~1;
            if (tw > w_) {
                tw = w_ & ~1;
            }
            if (th > h_) {
                th = h_ & ~1;
            }
            if (tw < 320 || th < 240) {
                Log("[提示] --size %dx%d 太小, 已忽略", wantW, wantH);
            } else {
                sizeMode_ = true;
                ow_ = tw;
                oh_ = th;
                std::string err;
                if (gpu_.Init(dev_.Get(), ctx_.Get(), w_, h_, ow_, oh_, &err)) {
                    useGpu_ = true;
                    how = "GPU 缩放";
                } else {
                    useGpu_ = false;
                    how = "CPU 缩放(GPU 不可用)";
                    Log("[提示] GPU 路径不可用(%s), 回退 CPU 双线性缩放", err.c_str());
                }
            }
        }

        if (!sizeMode_) {
            // 整数降采样倍数: 必须整除桌面尺寸, 且降完的宽高为偶数(UV 是 2x2 取样)
            if (scale == 2 || scale == 4) {
                if (w_ % scale == 0 && h_ % scale == 0 && (w_ / scale) % 2 == 0 &&
                    (h_ / scale) % 2 == 0) {
                    scale_ = scale;
                } else {
                    Log("[提示] --scale %d 不整除 %dx%d, 已忽略(改用 1)", scale, w_, h_);
                }
            } else if (scale != 1) {
                Log("[提示] --scale 只支持 1/2/4, 已忽略 %d", scale);
            }
            ow_ = w_ / scale_;
            oh_ = h_ / scale_;
            if (scale_ > 1) {
                how = "块平均降采样";
            }
        }

        Log("[dbg] staging OK, buffers...");
        // 中间缓冲始终按"桌面尺寸"分配; 缩放只影响最终送编码器的 nv12s_
        baseBgra_.resize((size_t)w_ * h_ * 4);
        workBgra_.resize((size_t)w_ * h_ * 4);
        nv12_.resize((size_t)w_ * h_ * 3 / 2);
        uPlane_.resize((size_t)w_ * h_);
        vPlane_.resize((size_t)w_ * h_);
        // 注意: --size 可能恰好等于源尺寸(如 2560x1600 全尺寸), 此时也必须分配 —— GPU 直接
        // 往这块缓冲写, 不分配就是越界写, 会踩坏堆。
        nv12s_.resize((size_t)ow_ * oh_ * 3 / 2);
        cursorBuf_.resize(4096);
        *outW = ow_;
        *outH = oh_;
        Log("捕获输出 #%d: %.*ls @ %ls  %dx%d → 推流 %dx%d [%s] (纯映射, 无旋转)", sel->index,
            (int)sel->deviceName.size(), sel->deviceName.c_str(), sel->adapterName.c_str(), w_, h_, ow_, oh_,
            how.c_str());
        return true;
    }

    // 拉取/复用一帧, 返回是否更新了 NV12
    bool Tick(int waitMs)
    {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(waitMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // 无新帧: 若鼠标位置/形状有变仍需重绘
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            Log("[警告] 桌面复制通道丢失, 重建中...");
            if (!RecreateDup()) {
                return false;
            }
            return Tick(0);
        } else if (SUCCEEDED(hr)) {
            // 先更新鼠标状态 —— 它决定本帧走 GPU 快路还是 CPU 合成路, 所以必须在使用源纹理之前。
            // (GetFramePointerShape 也要求在 ReleaseFrame 之前调用, 提前正好符合要求。)
            UpdateCursor(info);
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                if (td.Width == (UINT)srcW_ && td.Height == (UINT)srcH_) {
                    auto tM0 = Clock::now();
                    const bool needCursor = cursorVisible_ && cursor_.w > 0 && cursor_.h > 0;
                    gpuThisFrame_ = false;
                    if (useGpu_ && !needCursor) {
                        // GPU 快路: BGRA→NV12 且缩放到目标尺寸一步完成, 只回读缩小后的 NV12。
                        // 鼠标不在本屏时走这条(绝大多数帧)。
                        if (gpu_.Convert(tex.Get(), nv12s_.data())) {
                            gpuThisFrame_ = true;
                            gpuFrames_.fetch_add(1);
                            frameN_.fetch_add(1); // GPU 帧不做 CPU 合成/转换, 计入分母以摊薄平均值
                        } else {
                            gpuFail_.fetch_add(1);
                        }
                    }
                    if (!gpuThisFrame_) {
                        // CPU 路: 需要合成鼠标, 或 GPU 路径刚失败。回读整幅 BGRA。
                        ctx_->CopyResource(staging_.Get(), tex.Get());
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                            // 纯映射: 逐行直接搬, 不旋转、不裁切
                            const uint8_t *p = (const uint8_t *)m.pData;
                            for (int y = 0; y < h_; y++) {
                                memcpy(baseBgra_.data() + (size_t)y * w_ * 4, p + (size_t)y * m.RowPitch,
                                       (size_t)w_ * 4);
                            }
                            ctx_->Unmap(staging_.Get(), 0);
                            baseValid_ = true; // baseBgra_ 已是"本帧"内容
                        }
                    } else {
                        baseValid_ = false; // 本帧没回读 BGRA, 之前那份已经过期
                    }
                    haveFrame_ = true;
                    frameDirty_ = true;
                    // 读回 + 搬运耗时(含 GPU→CPU 传输), 是延迟的大头嫌疑
                    mapUs_.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                         Clock::now() - tM0)
                                         .count());
                } else if (!modeChanged_) {
                    // 用户在 Windows 显示设置里改了分辨率。若不处理, 尺寸不匹配会让帧被
                    // 静默丢弃 —— 表现就是"画面永久冻结"。置标志让上层重建本轮会话。
                    modeChanged_ = true;
                    Log("[提示] 实际帧尺寸变为 %ux%u (本会话期望 %dx%d), 需要按新尺寸重建捕获",
                        td.Width, td.Height, srcW_, srcH_);
                }
            }
            dup_->ReleaseFrame();
        } else {
            return false;
        }

        if (!haveFrame_) {
            return false;
        }
        bool cursorChanged = (cursorVisible_ != lastCursorVisible_) || (cursorPos_.x != lastCursorPos_.x) ||
                             (cursorPos_.y != lastCursorPos_.y) || shapeDirty_;
        if (!frameDirty_ && !cursorChanged) {
            return false; // 画面与鼠标都没变, 复用上次 NV12
        }
        frameDirty_ = false;
        shapeDirty_ = false;
        lastCursorVisible_ = cursorVisible_;
        lastCursorPos_ = cursorPos_;

        // GPU 已经在拉帧阶段完成转换与缩放, 这一帧到此为止。
        if (gpuThisFrame_) {
            return true;
        }

        // 计时: 把每帧耗时拆成"鼠标合成"与"BGRA→NV12 转换"两段, 用于定位瓶颈。
        auto tA = Clock::now();
        const uint8_t *srcBgra = baseBgra_.data();
        workValid_ = false;
        if (cursorVisible_ && cursor_.w > 0 && cursor_.h > 0) {
            if (!baseValid_) {
                // 手头没有"本帧"的 BGRA(GPU 帧不回读), 宁可少画这一拍鼠标,
                // 也不要把过期画面当底色合成 —— 那会让画面闪回上一次的内容。
                return false;
            }
            memcpy(workBgra_.data(), baseBgra_.data(), baseBgra_.size());
            DrawCursor();
            srcBgra = workBgra_.data();
            workValid_ = true;
        } else if (!baseValid_) {
            return false; // 既没有新回读也没有可复用的底图
        }
        auto tB = Clock::now();
        BgraToNv12(srcBgra, (size_t)w_ * 4, nv12_.data(), uPlane_.data(), vPlane_.data(), w_, h_);
        if (sizeMode_) {
            // --size: 任意尺寸, GPU 不可用(或该帧要合成鼠标)时走 CPU 双线性
            Nv12ScaleBilinear(nv12_.data(), w_, h_, nv12s_.data(), ow_, oh_);
        } else if (scale_ > 1) {
            // 复用已验证的原始尺寸转换, 再做整数倍块平均降采样 —— 少一条出错路径
            Nv12Downscale(nv12_.data(), w_, h_, nv12s_.data(), scale_);
        }
        addTiming(tA, tB, Clock::now());
        return true;
    }

    // 送进编码器的那份 NV12: 经过缩放/GPU 转换的在 nv12s_, 否则是全尺寸直通
    const uint8_t *Nv12() const
    {
        return (sizeMode_ || scale_ > 1) ? nv12s_.data() : nv12_.data();
    }
    // GPU 路径的帧数(用于日志确认真的走了 GPU) / 连续失败次数
    uint64_t GpuFrames() const { return gpuFrames_.load(); }
    uint64_t GpuFails() const { return gpuFail_.load(); }
    bool GpuActive() const { return useGpu_; }
    // 诊断用: 导出时给 Python 把 RGB 导回来对照(与送进编码器的那份内容一致)
    const uint8_t *Bgra() const { return workValid_ ? workBgra_.data() : baseBgra_.data(); }
    // BGRA 是否可用: GPU 路径的帧不回读 BGRA, 此时内容已过期, 不能拿去对照
    bool HasBgra() const { return workValid_ || baseValid_; }
    int FullWidth() const { return w_; }
    int FullHeight() const { return h_; }
    // ---- 性能计时(累计值, 单位微秒; frameN 是"真正做了转换的帧数") ----
    // 用来把每帧耗时拆成: 读回搬运 / 鼠标合成 / BGRA→NV12 转换 三段, 定位瓶颈。
    uint64_t MapUs() const { return mapUs_.load(); }
    uint64_t CurUs() const { return curUs_.load(); }
    uint64_t ConvUs() const { return convUs_.load(); }
    uint64_t FrameN() const { return frameN_.load(); }
    void ResetTiming()
    {
        mapUs_ = 0, curUs_ = 0, convUs_ = 0, frameN_ = 0;
    }
    int Width() const { return ow_; }
    int Height() const { return oh_; }
    bool HasFrame() const { return haveFrame_; }
    // 捕获源的分辨率在运行中被改过(用户在显示设置里调整)。上层应据此重建会话,
    // 否则尺寸不匹配会让所有帧被丢弃、画面永久冻结。
    bool ModeChanged() const { return modeChanged_; }
    void ForceDirty() { frameDirty_ = true; } // 仅捕获线程调用

    // 显式释放全部捕获资源。必须在移除所捕获的显示器(虚拟显示器)之前调用,
    // 否则 DXGI 复制通道会引用一个已消失的输出。
    void Shutdown()
    {
        gpu_.ResetInputView();
        dup_.Reset();
        output_.Reset();
        staging_.Reset();
        ctx_.Reset();
        dev_.Reset();
        haveFrame_ = false;
        modeChanged_ = false;
    }

    // 计时累加: 鼠标合成段 / BGRA→NV12 转换段
    void addTiming(const Clock::time_point &a, const Clock::time_point &b, const Clock::time_point &c)
    {
        auto us = [](const Clock::time_point &p, const Clock::time_point &q) {
            return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(q - p).count();
        };
        curUs_.fetch_add(us(a, b));
        convUs_.fetch_add(us(b, c));
        frameN_.fetch_add(1);
    }

private:
    bool RecreateDup()
    {
        dup_.Reset();
        gpu_.ResetInputView(); // 旧纹理作废, 输入视图必须跟着重建
        for (int i = 0; i < 10 && !g_exit.load(); i++) {
            HRESULT hr = output_->DuplicateOutput(dev_.Get(), &dup_);
            if (SUCCEEDED(hr)) {
                return true;
            }
            if (hr == E_ACCESSDENIED) {
                Log("[错误] 桌面复制被占用(其他程序正在捕获?), 2 秒后重试 %d/10", i + 1);
            } else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                Log("[错误] 复制通道数量达上限, 2 秒后重试 %d/10", i + 1);
            } else if (hr == E_NOTIMPL) {
                Log("[错误] 该输出不支持桌面复制");
                return false;
            } else {
                Log("[警告] DuplicateOutput 失败: 0x%08lX, 重试 %d/10", hr, i + 1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
        Log("[错误] 桌面复制初始化失败");
        return false;
    }

    void UpdateCursor(const DXGI_OUTDUPL_FRAME_INFO &info)
    {
        cursorVisible_ = info.PointerPosition.Visible != 0;
        cursorPos_ = info.PointerPosition.Position;
        if (info.PointerShapeBufferSize > 0) {
            UINT need = info.PointerShapeBufferSize;
            if (cursorBuf_.size() < need) {
                cursorBuf_.resize(need);
            }
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
            UINT got = 0;
            if (SUCCEEDED(dup_->GetFramePointerShape((UINT)cursorBuf_.size(), cursorBuf_.data(), &got, &si))) {
                cursor_.type = si.Type;
                cursor_.w = si.Width;
                cursor_.h = si.Height;
                cursor_.pitch = si.Pitch;
                cursor_.hot = si.HotSpot;
                cursor_.data.assign(cursorBuf_.data(), cursorBuf_.data() + got);
                shapeDirty_ = true;
            }
        }
    }

    void DrawCursor()
    {
        int cx = cursorPos_.x - cursor_.hot.x - outRect_.left;
        int cy = cursorPos_.y - cursor_.hot.y - outRect_.top;
        int cw = (int)cursor_.w;
        int ch = (int)cursor_.h;
        if (cx >= w_ || cy >= h_ || cx + cw <= 0 || cy + ch <= 0) {
            return;
        }
        const size_t pitch = (size_t)w_ * 4;
        if (cursor_.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
            cursor_.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            const uint8_t *s = cursor_.data.data();
            for (int r = 0; r < ch; r++) {
                int dy = cy + r;
                if (dy < 0 || dy >= h_) {
                    continue;
                }
                const uint8_t *row = s + (size_t)r * cursor_.pitch;
                uint8_t *d = workBgra_.data() + (size_t)dy * pitch;
                for (int c = 0; c < cw; c++) {
                    int dx = cx + c;
                    if (dx < 0 || dx >= w_) {
                        continue;
                    }
                    uint8_t B = row[c * 4 + 0], G = row[c * 4 + 1], R = row[c * 4 + 2], A = row[c * 4 + 3];
                    uint8_t *p = d + (size_t)dx * 4;
                    if (A == 0) {
                        continue;
                    }
                    if (A == 255) {
                        p[0] = B;
                        p[1] = G;
                        p[2] = R;
                    } else { // 预乘 alpha 混合
                        p[0] = (uint8_t)(B + p[0] * (255 - A) / 255);
                        p[1] = (uint8_t)(G + p[1] * (255 - A) / 255);
                        p[2] = (uint8_t)(R + p[2] * (255 - A) / 255);
                    }
                }
            }
        } else { // MONO
            const uint8_t *andMask = cursor_.data.data();
            const uint8_t *xorMask = cursor_.data.data() + (size_t)cursor_.pitch * ch;
            for (int r = 0; r < ch; r++) {
                int dy = cy + r;
                if (dy < 0 || dy >= h_) {
                    continue;
                }
                const uint8_t *arow = andMask + (size_t)r * cursor_.pitch;
                const uint8_t *xrow = xorMask + (size_t)r * cursor_.pitch;
                uint8_t *d = workBgra_.data() + (size_t)dy * pitch;
                for (int c = 0; c < cw; c++) {
                    int dx = cx + c;
                    if (dx < 0 || dx >= w_) {
                        continue;
                    }
                    uint8_t ab = arow[c >> 3] & (0x80u >> (c & 7));
                    uint8_t xb = xrow[c >> 3] & (0x80u >> (c & 7));
                    uint8_t *p = d + (size_t)dx * 4;
                    if (!ab && !xb) {
                        p[0] = p[1] = p[2] = 0;
                    } else if (!ab && xb) {
                        p[0] = p[1] = p[2] = 255;
                    } else if (ab && xb) {
                        p[0] = (uint8_t)~p[0];
                        p[1] = (uint8_t)~p[1];
                        p[2] = (uint8_t)~p[2];
                    }
                }
            }
        }
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
    GpuScaler gpu_;              // GPU 色彩转换 + 缩放(--size 时启用)
    bool useGpu_ = false;        // GPU 路径是否可用且已启用
    bool sizeMode_ = false;      // 是否用 --size 指定了推流尺寸
    bool gpuThisFrame_ = false;  // 本帧是否已由 GPU 转好(决定 Tick 后半段是否还要做 CPU 转换)
    bool baseValid_ = false;     // baseBgra_ 是否为"本帧"内容(GPU 帧不回读, 会置 false)
    std::atomic<uint64_t> gpuFrames_{0}, gpuFail_{0};
    int w_ = 0, h_ = 0;
    RECT outRect_{};
    std::vector<uint8_t> baseBgra_, workBgra_, nv12_, uPlane_, vPlane_, cursorBuf_;
    std::vector<uint8_t> nv12s_;   // 降采样后的 NV12(--scale 2/4 时才用)
    int srcW_ = 0, srcH_ = 0;      // DDA 实际交出来的帧尺寸(与 w_/h_ 相同; 纯映射模式)
    int ow_ = 0, oh_ = 0;          // 推流尺寸 = 画布尺寸 / scale
    int scale_ = 1;                // 整数降采样倍数: 1/2/4
    bool haveFrame_ = false;
    bool frameDirty_ = false;
    bool shapeDirty_ = false;
    bool modeChanged_ = false;
    bool workValid_ = false; // workBgra_ 是否是本帧的有效拷贝(仅在需要合成鼠标时)
    // 性能计时累加器(捕获线程写, 统计线程读)
    std::atomic<uint64_t> mapUs_{0}, curUs_{0}, convUs_{0}, frameN_{0};
    bool cursorVisible_ = false;
    bool lastCursorVisible_ = false;
    POINT cursorPos_{};
    POINT lastCursorPos_{-10000, -10000};
    struct
    {
        UINT type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        UINT w = 0, h = 0, pitch = 0;
        POINT hot{};
        std::vector<uint8_t> data;
    } cursor_;
};

// ============================ H.264 编码器(MF MFT, 支持异步硬编) ============================

class H264Encoder
{
public:
    using AuHandler = std::function<void(const uint8_t *, size_t)>;

    bool Create(int w, int h, int fps, int kbps, const std::wstring &prefer, AuHandler onAu,
                bool sysmemInput)
    {
        w_ = w;
        h_ = h;
        fps_ = fps;
        kbps_ = kbps;
        onAu_ = std::move(onAu);
        fixer_ = AuFixer();
        d3dFault_ = false;

        MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_H264};

        struct Cand
        {
            ComPtr<IMFActivate> act;
            std::wstring name;
            bool hw = false;
        };
        std::vector<Cand> cands;
        auto collect = [&](DWORD flags, bool hw) {
            IMFActivate **arr = nullptr;
            UINT32 count = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER,
                                 &inInfo, &outInfo, &arr, &count))) {
                return;
            }
            for (UINT32 i = 0; i < count; i++) {
                Cand c;
                c.hw = hw;
                wchar_t name[256] = L"(unknown)";
                arr[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
                c.name = name;
                c.act = arr[i];
                arr[i]->Release();
                cands.push_back(std::move(c));
            }
            CoTaskMemFree(arr);
        };
        collect(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT, true);
        if (cands.empty()) {
            collect(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT, false);
        }

        // 优先级而非过滤: 匹配 prefer 的排前面, 其余按原顺序殿后 —— 保证回退链完整
        // (老逻辑是硬过滤, 首选编码器初始化失败时整个链就断了)。
        std::vector<Cand> tries;
        std::vector<bool> used(cands.size(), false);
        if (!prefer.empty()) {
            for (size_t i = 0; i < cands.size(); i++) {
                if (wcsstr(cands[i].name.c_str(), prefer.c_str()) != nullptr) {
                    tries.push_back(std::move(cands[i]));
                    used[i] = true;
                }
            }
        }
        for (size_t i = 0; i < cands.size(); i++) {
            if (!used[i]) {
                tries.push_back(std::move(cands[i]));
            }
        }
        if (tries.empty()) {
            Log("[错误] 系统没有任何可用的 H.264 编码器");
            return false;
        }

        // ⭐ 输入通道策略(2026-09-15 泄漏彻查):
        //   旧路径 = 每帧 MFCreateMemoryBuffer 系统内存块喂 MFT —— 实测 QSV 以此方式
        //   每帧泄漏 ~4.7MB 提交内存(190 秒吃光 40GB 配额, 且不随编码器销毁回收)。
        //   新路径 = 给 MFT 挂它所在 GPU 的 D3D 设备(MFT_MESSAGE_SET_D3D_MANAGER),
        //   输入直接用 GPU 纹理(MFCreateDXGISurfaceBuffer), 绕开驱动对系统内存的逐帧上传。
        //   每个硬件候选先试 D3D 路径, D3D 通道自身故障则该候选自动退回系统内存路径。
        bool d3dBanned = false;
        for (auto &c : tries) {
            Log("尝试编码器: %ls%s", c.name.c_str(), c.hw ? " (硬件)" : "");
            if (c.hw && !sysmemInput && !d3dBanned) {
                if (TrySetup(c.act.Get(), c.name, w, h, fps, kbps, true)) {
                    running_ = true;
                    thread_ = std::thread(&H264Encoder::EventLoop, this);
                    Log("编码器就绪: %ls | %d×%d@%dfps %dkbps %s%s", c.name.c_str(), w, h, fps, kbps,
                        async_ ? "异步" : "同步", d3dInput_ ? " | D3D纹理输入" : "");
                    return true;
                }
                ResetPartial();
                if (d3dFault_) {
                    d3dBanned = true; // 是 D3D 通道的问题, 别在每个候选上重蹈覆辙
                }
            }
            if (TrySetup(c.act.Get(), c.name, w, h, fps, kbps, false)) {
                running_ = true;
                thread_ = std::thread(&H264Encoder::EventLoop, this);
                Log("编码器就绪: %ls | %d×%d@%dfps %dkbps %s", c.name.c_str(), w, h, fps, kbps,
                    async_ ? "异步" : "同步");
                return true;
            }
            ResetPartial();
        }
        Log("[错误] 所有 H.264 编码器均初始化失败");
        return false;
    }

    bool TrySetup(IMFActivate *act, const std::wstring &name, int w, int h, int fps, int kbps,
                  bool useD3D)
    {
        // 低延迟必须在 ActivateObject **之前**设到 IMFActivate 上, 这样驱动在初始化时
        // 就按低延迟配置(不做多帧 lookahead 缓冲)。激活后再设 MFT 属性作为双保险。
        // 为什么不能只靠 CODECAPI_AVEncCommonRealTime: 本机 QSV 对它返回"不支持", 会被忽略。
        act->SetUINT32(MF_LOW_LATENCY, TRUE);
        HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) {
            // NVIDIA MFT 实测(2026-09-15): 带着预激活设置激活报 0x8000FFFF。
            // 清掉重试一次(低延迟由激活后的 MFT 属性承担, 不受影响)。
            mft_.Reset();
            act->DeleteItem(MF_LOW_LATENCY);
            hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
            if (FAILED(hr)) {
                Log("  激活失败: 0x%08lX", hr);
                return false;
            }
            Log("  (去掉预激活设置后激活成功)");
        }
        ComPtr<IMFAttributes> attrs;
        async_ = false;
        if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
            UINT32 asyncFlag = 0;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
            async_ = asyncFlag != 0;
            if (async_) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE); // 双保险, 且必须在下面 SetOutputType 之前
        }

        // D3D 纹理输入通道: 必须在 SetOutputType/SetInputType 之前挂上设备管理器。
        // 失败不致命 —— 自动退回系统内存输入路径(旧行为)。
        d3dInput_ = false;
        if (useD3D && SetupD3DManager(name)) {
            d3dInput_ = true;
        }

        // 输出类型(H264) — 编码器需先设输出再设输入
        ComPtr<IMFMediaType> mtOut;
        MFCreateMediaType(&mtOut);
        mtOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mtOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        mtOut->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
        mtOut->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT64)h);
        mtOut->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)fps << 32) | 1);
        mtOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mtOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = mft_->SetOutputType(0, mtOut.Get(), 0);
        if (FAILED(hr)) {
            // 挑剔的编码器(实测 Microsoft AVC DX12 Encoder 报 0x80041000): 不猜它要什么,
            // 直接枚举它自己提供的输出类型, 逐个试, 只把码率改成我们的值。
            bool ok = false;
            for (UINT32 ti = 0; ti < 24; ti++) {
                ComPtr<IMFMediaType> avail;
                if (FAILED(mft_->GetOutputAvailableType(0, ti, &avail)) || !avail) {
                    break;
                }
                avail->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
                if (SUCCEEDED(mft_->SetOutputType(0, avail.Get(), 0))) {
                    ok = true;
                    Log("  (使用编码器提供的输出类型 #%u)", ti);
                    break;
                }
            }
            if (!ok) {
                Log("  设置输出格式失败: 0x%08lX", hr);
                return false;
            }
        }

        ComPtr<IMFMediaType> mtIn;
        MFCreateMediaType(&mtIn);
        mtIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mtIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        mtIn->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT64)h);
        mtIn->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)fps << 32) | 1);
        mtIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mtIn->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w);
        hr = mft_->SetInputType(0, mtIn.Get(), 0);
        if (FAILED(hr)) {
            Log("  设置输入格式失败: 0x%08lX", hr);
            return false;
        }

        // D3D 通道的上传资源(纹理池 + staging)。创建失败视为 D3D 通道故障,
        // 让上层以系统内存路径重试同一候选。
        if (d3dInput_ && !InitUploadPool(w, h)) {
            Log("  [enc] D3D 上传池创建失败(标记 D3D 通道故障)");
            d3dFault_ = true;
            return false;
        }

        // 编码参数(尽力设置, 失败不致命)
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) {
            auto setU32 = [&api](const GUID &prop, UINT32 v, const char *name) {
                VARIANT var;
                VariantInit(&var);
                var.vt = VT_UI4;
                var.ulVal = v;
                HRESULT r = api->SetValue(&prop, &var);
                VariantClear(&var);
                if (FAILED(r)) {
                    Log("  编码参数 %s = %u: 不支持(忽略)", name, v);
                }
            };
            setU32(CODECAPI_AVLowLatencyMode, 1, "低延迟");
            setU32(CODECAPI_AVEncCommonRealTime, 1, "实时优先");
            setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "CBR");
            setU32(CODECAPI_AVEncCommonMeanBitRate, (UINT32)kbps * 1000, "码率");
            setU32(CODECAPI_AVEncMPVGOPSize, (UINT32)fps, "GOP(1秒一个关键帧, 缩短重连恢复时间)");
            setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "B帧");
        }

        if (async_) {
            if (FAILED(mft_.As(&meg_))) {
                Log("  异步编码器缺少事件接口");
                return false;
            }
        }
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        return true;
    }

    void ResetPartial()
    {
        mft_.Reset();
        meg_.Reset();
        ownBuf_.Reset();
        upPool_.clear();
        upStaging_.Reset();
        upMgr_.Reset();
        upCtx_.Reset();
        upDev_.Reset();
        d3dInput_ = false;
        async_ = false;
    }

    void Destroy()
    {
        if (!mft_) {
            return;
        }
        running_ = false;
        queue_.Close();
        if (thread_.joinable()) {
            thread_.join();
        }
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        meg_.Reset();
        mft_.Reset();
        // MFT 先于设备/管理器释放(它还引用着设备)
        upPool_.clear();
        upStaging_.Reset();
        upMgr_.Reset();
        upCtx_.Reset();
        upDev_.Reset();
    }

    void PushFrame(FrameBuf &&f) { queue_.Push(std::move(f)); }
    FrameQueue *Queue() { return &queue_; }
    uint64_t Encoded() const { return encoded_.load(); }
    uint64_t Dropped() const { return queue_.Dropped(); }
    // 捕获 → 编码出 AU 的平均延迟(毫秒)。用来判断瓶颈在发送端内部还是别处。
    double AvgLatencyMs() const
    {
        uint64_t c = latCnt_.load();
        return c ? (double)latSumUs_.load() / (double)c / 1000.0 : 0.0;
    }

private:
    void EventLoop()
    {
      try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        if (async_) {
            while (running_.load()) {
                ComPtr<IMFMediaEvent> ev;
                HRESULT hr = meg_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
                if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                    // 轮询间隔就是"每帧白等的延迟": 原来是 2ms, 而输入/输出每个来回都要等一次,
                    // 一帧下来能白攒好几毫秒。降到 200µs(反正只是空转检查, 不占 CPU)。
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                if (FAILED(hr)) {
                    if (dbgErr_++ < 10) {
                        Log("  [enc] GetEvent 失败: 0x%08lX", hr);
                    }
                    break;
                }
                MediaEventType type = MEUnknown;
                ev->GetType(&type);
                if (type == METransformNeedInput) {
                    dbgNeed_++;
                    if (!FeedOne()) {
                        continue;
                    }
                } else if (type == METransformHaveOutput) {
                    dbgHave_++;
                    PumpOutput();
                } else if (type == MEError) {
                    // 事件对象上带着失败 HRESULT(驱动/编码器的真实死因), 必须取出来
                    HRESULT st = S_OK;
                    ev->GetStatus(&st);
                    Log("[错误] 编码器事件错误(hr=0x%08lX), 流中断 [fed=%llu out=%llu backp=%llu]",
                        (unsigned long)st, (unsigned long long)fed_,
                        (unsigned long long)encoded_.load(), backp_);
                    ProbeLog("enc MEError hr=0x%08lX fed=%llu out=%llu backp=%llu commit=%lluMB",
                             (unsigned long)st, (unsigned long long)fed_,
                             (unsigned long long)encoded_.load(), backp_, ProbeCommitMB());
                    streamDead_ = true;
                    break;
                }
            }
        } else {
            while (running_.load()) {
                FrameBuf f;
                if (!queue_.Pop(f)) {
                    break;
                }
                ComPtr<IMFSample> s;
                if (!MakeSample(f, &s)) {
                    continue;
                }
                if (FAILED(mft_->ProcessInput(0, s.Get(), 0))) {
                    continue;
                }
                while (running_.load()) {
                    int r = PumpOutput();
                    if (r != 0) {
                        break;
                    }
                }
            }
        }
        streamDead_ = true;
      } catch (const std::bad_alloc &) {
        ProbeMem("EventLoop");
        streamDead_ = true;
      } catch (const std::exception &e) {
        ProbeLog("exception in EventLoop: %s", e.what());
        streamDead_ = true;
      } catch (...) {
        ProbeLog("non-std exception in EventLoop");
        streamDead_ = true;
      }
    }

    bool FeedOne()
    {
        FrameBuf f;
        if (!queue_.Pop(f)) {
            return false;
        }
        ComPtr<IMFSample> s;
        if (!MakeSample(f, &s)) {
            if (dbgErr_++ < 10) {
                Log("  [enc] MakeSample 失败");
            }
            return false;
        }
        HRESULT hr = mft_->ProcessInput(0, s.Get(), 0);
        if (FAILED(hr)) {
            if (dbgErr_++ < 10) {
                Log("  [enc] ProcessInput 失败: 0x%08lX (第 %llu 帧)", hr,
                    (unsigned long long)(dbgNeed_));
            }
            return false;
        }
        latQ_.push_back(f.capT); // 记住这帧的捕获时刻, 出 AU 时算延迟(编码器 1 进 1 出, FIFO 对应)
        fed_++;                  // 在途护栏计数(ProcessInput 成功才算喂入)
        return true;
    }

    // ---- D3D 纹理输入通道 ----
    // 给 MFT 挂一块它所在 GPU(Intel=QSV / NVIDIA=NVENC)的 D3D11 设备。
    // 之后输入样本 = GPU 纹理(MFCreateDXGISurfaceBuffer), MFT 零拷贝取用;
    // 旧路径的"每帧 MFCreateMemoryBuffer + 驱动内部逐帧上传 surface"不再发生 —— 那是
    // 泄漏嫌疑最大的一环(每帧 ~4.7MB 提交内存, 只涨不落)。
    bool SetupD3DManager(const std::wstring &name)
    {
        const wchar_t *wantW = nullptr;
        if (wcsstr(name.c_str(), L"Intel") != nullptr) {
            wantW = L"Intel"; // QSV 在核显上
        } else if (wcsstr(name.c_str(), L"NVIDIA") != nullptr) {
            wantW = L"NVIDIA";
        } else if (wcsstr(name.c_str(), L"Microsoft") != nullptr) {
            // Microsoft AVC DX12 Encoder: 疑似必须在 SetOutputType 前拿到设备
            // (不带设备时 SetOutputType 报 0x80041000)。RTX 5070 有 DX12 编码能力。
            wantW = L"NVIDIA";
        } else {
            return false;
        }
        char want[16]{};
        WideCharToMultiByte(CP_UTF8, 0, wantW, -1, want, sizeof(want) - 1, nullptr, nullptr);

        ComPtr<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            return false;
        }
        ComPtr<IDXGIAdapter1> ad;
        for (UINT i = 0; f->EnumAdapters1(i, &ad) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            char desc[128]{};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, desc, sizeof(desc) - 1, nullptr,
                                nullptr);
            if (strstr(desc, want) != nullptr) {
                break;
            }
            ad.Reset();
        }
        if (!ad) {
            Log("  [enc] 找不到 %ls 适配器, 走系统内存输入", wantW);
            return false;
        }
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (FAILED(D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                                     D3D11_SDK_VERSION, &upDev_, nullptr, &upCtx_))) {
            return false;
        }
        // 交给 MF 的设备必须开多线程保护: MFT 内部线程与我们的上传共用这台设备
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(upCtx_.As(&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }
        if (FAILED(MFCreateDXGIDeviceManager(&upMgrToken_, &upMgr_)) ||
            FAILED(upMgr_->ResetDevice(upDev_.Get(), upMgrToken_)) ||
            FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                        (ULONG_PTR)upMgr_.Get()))) {
            Log("  [enc] 挂 D3D 设备管理器失败, 走系统内存输入");
            upMgr_.Reset();
            upCtx_.Reset();
            upDev_.Reset();
            return false;
        }
        Log("  [enc] 已挂 %ls GPU 的 D3D 设备(纹理输入通道)", wantW);
        return true;
    }

    // 上传资源: staging(CPU 写) → CopyResource → 池中 DEFAULT 纹理(MFT 直接引用)。
    // 池 32 张轮转 + 在途护栏(>16 丢帧), 保证绝不覆盖仍被 MFT 引用的纹理。
    bool InitUploadPool(int w, int h)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET; // NV12 RT 已在 GpuScaler 验证; 不行再降级 0
        upPool_.resize(kUploadPool);
        for (auto &t : upPool_) {
            if (FAILED(upDev_->CreateTexture2D(&td, nullptr, &t))) {
                td.BindFlags = 0;
                if (FAILED(upDev_->CreateTexture2D(&td, nullptr, &t))) {
                    return false;
                }
            }
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(upDev_->CreateTexture2D(&td, nullptr, &upStaging_))) {
            return false;
        }
        return true;
    }

    bool MakeSampleGpu(const FrameBuf &f, ComPtr<IMFSample> *out)
    {
        const size_t need = (size_t)w_ * h_ * 3 / 2;
        if (f.nv12.size() < need) {
            return false;
        }
        // 在途护栏: 编码器未产出的帧超过池一半时丢帧(宁可丢也不覆盖在用纹理)。
        // 若此计数持续增长且 out 不动 = MFT 扣着纹理不放, 护栏把它变成可见的"丢帧"。
        if (fed_ - encoded_.load() > kUploadPool / 2) {
            backp_++;
            if ((backp_ % 256) == 1) {
                ProbeLog("enc backpressure: inflight=%llu (fed=%llu out=%llu)",
                         (unsigned long long)(fed_ - encoded_.load()),
                         (unsigned long long)fed_, (unsigned long long)encoded_.load());
            }
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mr{};
        if (FAILED(upCtx_->Map(upStaging_.Get(), 0, D3D11_MAP_WRITE, 0, &mr)) || !mr.pData) {
            return false;
        }
        const uint8_t *src = f.nv12.data();
        uint8_t *dst = (uint8_t *)mr.pData;
        for (int y = 0; y < h_; y++) {
            memcpy(dst + (size_t)y * mr.RowPitch, src + (size_t)y * w_, (size_t)w_);
        }
        uint8_t *dstUV = dst + (size_t)mr.RowPitch * h_; // NV12: UV 平面偏移 = RowPitch × 高
        const uint8_t *srcUV = src + (size_t)w_ * h_;
        for (int y = 0; y < h_ / 2; y++) {
            memcpy(dstUV + (size_t)y * mr.RowPitch, srcUV + (size_t)y * w_, (size_t)w_);
        }
        upCtx_->Unmap(upStaging_.Get(), 0);
        ComPtr<ID3D11Texture2D> tex = upPool_[upNext_];
        upNext_ = (upNext_ + 1) % upPool_.size();
        upCtx_->CopyResource(tex.Get(), upStaging_.Get());
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex.Get(), 0, FALSE, &b))) {
            return false;
        }
        ComPtr<IMFSample> s;
        if (FAILED(MFCreateSample(&s)) || FAILED(s->AddBuffer(b.Get()))) {
            return false;
        }
        s->SetSampleTime(f.ts);
        s->SetSampleDuration(f.dur);
        *out = s;
        return true;
    }

    bool MakeSample(const FrameBuf &f, ComPtr<IMFSample> *out)
    {
        if (d3dInput_) {
            return MakeSampleGpu(f, out);
        }
        ComPtr<IMFSample> s;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer((DWORD)f.nv12.size(), &b))) {
            return false;
        }
        BYTE *p = nullptr;
        if (FAILED(b->Lock(&p, nullptr, nullptr))) {
            return false;
        }
        memcpy(p, f.nv12.data(), f.nv12.size());
        b->Unlock();
        b->SetCurrentLength((DWORD)f.nv12.size());
        s->AddBuffer(b.Get());
        s->SetSampleTime(f.ts);
        s->SetSampleDuration(f.dur);
        *out = s;
        return true;
    }

    // 0: 得到输出; 1: 暂无更多输出; -1: 错误
    int PumpOutput()
    {
        MFT_OUTPUT_STREAM_INFO si{};
        mft_->GetOutputStreamInfo(0, &si);
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        ComPtr<IMFSample> own;
        if (!(si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            // 复用同一块输出缓冲(创建一次): 老代码每帧新建 4MB, 若 MFT 短暂持有旧块的引用
            // 就会按帧泄漏 —— 实测泄漏速率 ≈ 4.7MB/帧, 与这个 4MB 缓冲吻合。
            // ProcessOutput 是同步取走已编码数据, 我们读完再复用, 单飞行帧下安全。
            if (!ownBuf_) {
                DWORD sz = si.cbSize > 0 ? si.cbSize : (DWORD)(4 << 20);
                if (FAILED(MFCreateMemoryBuffer(sz, &ownBuf_))) {
                    return -1;
                }
            }
            if (FAILED(MFCreateSample(&own))) {
                return -1;
            }
            own->AddBuffer(ownBuf_.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return 1;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // 异步 MFT 要求: 变更后重设输出类型, 事件流才会恢复
            ComPtr<IMFMediaType> newType;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &newType)) && newType) {
                newType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps_ * 1000);
                HRESULT r2 = mft_->SetOutputType(0, newType.Get(), 0);
                if (dbgErr_++ < 10) {
                    Log("  [enc] 输出流变更 → 重设输出类型: 0x%08lX", r2);
                }
            }
            return 1;
        }
        if (FAILED(hr)) {
            if (dbgErr_++ < 10) {
                Log("  [enc] ProcessOutput 失败: 0x%08lX status=%lu", hr, status);
            }
            return -1;
        }
        // ⚠️⚠️ 内存泄漏根因(2026-09-14): QSV 异步 MFT 是"PROVIDES_SAMPLES"模式 —— 它把
        // 输出样本指针写进 ob.pSample 交给调用方, **契约要求调用方负责 Release**。
        // 老代码只拿 ConvertToContiguousBuffer 里的 buffer 用, ob.pSample 从未被释放
        // → 每帧泄漏一个 ~3.3MB 的 IMFSample → 8400 帧(~190 秒)后提交内存耗尽 → bad_alloc。
        // 这里必须用 ComPtr 接管引用, 作用域结束自动释放。(own 模式下 ob.pSample 就是
        // own.Get(), 接管只是引用+1, 同样正确。)
        ComPtr<IMFSample> outS = ob.pSample;
        if (!outS) {
            if (dbgNoOut_++ < 10) {
                Log("  [enc] ProcessOutput 成功但没有样本(忽略, 等下个事件, 第 %llu 次)",
                    (unsigned long long)dbgNoOut_);
            }
            return 1;
        }
        ComPtr<IMFMediaBuffer> cb;
        if (FAILED(outS->ConvertToContiguousBuffer(&cb))) {
            return -1;
        }
        BYTE *p = nullptr;
        DWORD len = 0;
        if (FAILED(cb->Lock(&p, nullptr, &len)) || len == 0) {
            return -1;
        }
        std::vector<uint8_t> au;
        fixer_.Process(p, len, au);
        cb->Unlock();
        if (onAu_) {
            onAu_(au.data(), au.size());
        }
        if (!latQ_.empty()) {
            auto t0 = latQ_.front();
            latQ_.pop_front();
            latSumUs_.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                    Clock::now() - t0)
                                    .count());
            latCnt_.fetch_add(1);
        }
        encoded_.fetch_add(1);
        return 0;
    }

public:
    bool StreamDead() const { return streamDead_.load(); }
    uint64_t DbgNeed() const { return dbgNeed_; }
    uint64_t DbgHave() const { return dbgHave_; }

private:
    int w_ = 0, h_ = 0, fps_ = 30, kbps_ = 0;
    bool async_ = false;
    // D3D 纹理输入通道(治 QSV 每帧 ~4.7MB 提交泄漏)
    static constexpr size_t kUploadPool = 32; // 纹理池大小; 在途护栏 = 一半
    ComPtr<ID3D11Device> upDev_;
    ComPtr<ID3D11DeviceContext> upCtx_;
    ComPtr<IMFDXGIDeviceManager> upMgr_;
    UINT upMgrToken_ = 0;
    ComPtr<ID3D11Texture2D> upStaging_;
    std::vector<ComPtr<ID3D11Texture2D>> upPool_;
    size_t upNext_ = 0;
    bool d3dInput_ = false; // 当前会话是否走 GPU 纹理输入
    bool d3dFault_ = false; // D3D 通道自身故障(设备/纹理创建失败) → 后续候选免试
    uint64_t fed_ = 0;      // 成功 ProcessInput 的帧数(在途护栏)
    uint64_t backp_ = 0;    // 因在途超限被丢的帧数
    // 延迟统计: latQ_ 只在编码线程(EventLoop)里读写, 所以不用锁;
    // 累加器是原子量, 供统计线程读取。
    std::deque<Clock::time_point> latQ_;
    std::atomic<uint64_t> latSumUs_{0};
    std::atomic<uint64_t> latCnt_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> streamDead_{false};
    std::atomic<uint64_t> encoded_{0};
    uint64_t dbgNeed_ = 0, dbgHave_ = 0, dbgErr_ = 0, dbgNoOut_ = 0;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> meg_;
    ComPtr<IMFMediaBuffer> ownBuf_;
    AuFixer fixer_;
    AuHandler onAu_;
    FrameQueue queue_;
    std::thread thread_;
};

// ============================ TCP 客户端 ============================

class TcpClient
{
public:
    bool Connect(const char *host, int port)
    {
        Close();
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        char portStr[16]{};
        snprintf(portStr, sizeof(portStr), "%d", port);
        if (getaddrinfo(host, portStr, &hints, &res) != 0 || res == nullptr) {
            return false;
        }
        fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ == INVALID_SOCKET) {
            freeaddrinfo(res);
            return false;
        }
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        if (connect(fd_, res->ai_addr, (int)res->ai_addrlen) != 0) {
            freeaddrinfo(res);
            Close();
            return false;
        }
        freeaddrinfo(res);
        return true;
    }

    bool SendAll(const void *data, size_t len)
    {
        const uint8_t *p = (const uint8_t *)data;
        while (len > 0) {
            int n = send(fd_, (const char *)p, (int)len, 0);
            if (n <= 0) {
                return false;
            }
            p += n;
            len -= (size_t)n;
        }
        return true;
    }

    void Close()
    {
        if (fd_ != INVALID_SOCKET) {
            closesocket(fd_);
            fd_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET fd_ = INVALID_SOCKET;
};

// ============================ hdc fport 管理 ============================

static std::string FindHdc(const std::string &arg)
{
    if (!arg.empty()) {
        return arg;
    }
    const char *candidates[] = {
        "C:\\Program Files\\Huawei\\DevEco Studio1\\sdk\\default\\openharmony\\toolchains\\hdc.exe",
        "C:\\Program Files\\Huawei\\DevEco Studio\\sdk\\default\\openharmony\\toolchains\\hdc.exe",
    };
    for (const char *c : candidates) {
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) {
            return c;
        }
    }
    char buf[MAX_PATH]{};
    if (SearchPathA(nullptr, "hdc.exe", nullptr, MAX_PATH, buf, nullptr) > 0) {
        return buf;
    }
    return "";
}

static bool RunHidden(const std::string &exe, const std::string &args)
{
    std::string cmd = "\"" + exe + "\" " + args;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 可被 Ctrl+C 打断的等待。守护模式里所有 sleep 都必须用它, 否则退出会卡很久。
static void SleepInterruptible(int ms)
{
    int left = ms;
    while (left > 0 && !g_exit.load()) {
        int chunk = (left > 50) ? 50 : left;
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        left -= chunk;
    }
}

// 执行子进程并捕获它的 stdout+stderr, 带超时(超时则强杀)。
// hdc 在"没有设备"时可能阻塞好几秒, 所以守护模式必须用带超时的版本, 不能裸等。
static bool RunCapture(const std::string &exe, const std::string &args, int timeoutMs, std::string *out)
{
    out->clear();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;

    std::string cmd = "\"" + exe + "\" " + args;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return false;
    }
    CloseHandle(wr); // 父进程只留读端, 否则读不到 EOF

    bool finished = (WaitForSingleObject(pi.hProcess, (DWORD)timeoutMs) == WAIT_OBJECT_0);
    if (!finished) {
        TerminateProcess(pi.hProcess, 1); // 超时: 例如 hdc 在无设备时长时间不返回
        WaitForSingleObject(pi.hProcess, 2000);
    }
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out->append(buf, got);
        if (out->size() > 65536) {
            break;
        }
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

// 判断一行 hdc 输出是否为真实设备标识。
// 实例: "4QFUN25308G00246" (USB 序列号, 注意**不是**十六进制) / "127.0.0.1:5555" (网络目标)。
// 必须排除 "Connect server failed" / "[Empty]" 之类的提示行, 否则会出现
// "屏幕出来了却连不上" 这种假阳性。
static bool IsDeviceLine(const std::string &line)
{
    std::string low;
    low.reserve(line.size());
    for (char c : line) {
        low.push_back((char)tolower((unsigned char)c));
    }
    if (low.find("empty") != std::string::npos || low.find("failed") != std::string::npos ||
        low.find("error") != std::string::npos || low.find("not found") != std::string::npos ||
        low.find("unknown") != std::string::npos) {
        return false;
    }
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return false;
    }
    size_t e = line.find_first_of(" \t", b);
    std::string tok = line.substr(b, (e == std::string::npos ? line.size() : e) - b);
    if (tok.size() < 6) {
        return false;
    }
    for (char c : tok) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '.' || c == ':' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// 平板是否在线(既能插入也会因 hdc 启动失败而判否)。
static bool HdcDevicePresent(const std::string &hdc)
{
    if (hdc.empty()) {
        return false;
    }
    std::string out;
    if (!RunCapture(hdc, "list targets", 4000, &out)) {
        return false; // 超时/启动失败 => 视为不在线
    }
    size_t i = 0;
    while (i < out.size()) {
        size_t j = out.find('\n', i);
        if (j == std::string::npos) {
            j = out.size();
        }
        std::string line = out.substr(i, j - i);
        i = j + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (IsDeviceLine(line)) {
            return true;
        }
    }
    return false;
}

static void EnsureFport(const std::string &hdc, int port)
{
    if (hdc.empty()) {
        return;
    }
    std::string dummy;
    char a[128], b[128];
    snprintf(a, sizeof(a), "fport rm tcp:%d tcp:%d", port, port);
    snprintf(b, sizeof(b), "fport tcp:%d tcp:%d", port, port);
    RunCapture(hdc, a, 4000, &dummy);
    RunCapture(hdc, b, 4000, &dummy);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

static void RemoveFport(const std::string &hdc, int port)
{
    if (hdc.empty()) {
        return;
    }
    std::string dummy;
    char a[128];
    snprintf(a, sizeof(a), "fport rm tcp:%d tcp:%d", port, port);
    RunCapture(hdc, a, 4000, &dummy);
}

// "在"不等于"可用": 只有 fport 真的列出来了才算链路就绪。
static bool FportActive(const std::string &hdc, int port)
{
    if (hdc.empty()) {
        return false;
    }
    std::string out;
    if (!RunCapture(hdc, "fport ls", 4000, &out)) {
        return false;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "tcp:%d", port);
    return out.find(pat) != std::string::npos;
}

// 平板端 App 是否在运行。用 ps 找进程名(包名) —— 比"设备在线"更能说明"App 真的起来了"。
static bool HdcAppRunning(const std::string &hdc, const char *bundle)
{
    if (hdc.empty() || bundle == nullptr) {
        return false;
    }
    std::string out;
    if (!RunCapture(hdc, "shell ps -ef", 6000, &out)) {
        return false;
    }
    return out.find(bundle) != std::string::npos;
}

// 平板端 App 的监听端口是否真的活着。
// 判据很硬: hdc fport 只有在**设备侧确实有进程在 listen** 时才能连上,
// 所以"TCP 三次握手成功"就等价于"App 的网络服务在跑"(比猜进程名可靠)。
static bool TcpPortAlive(const char *host, int port, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host, &a.sin_addr);

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr *)&a, sizeof(a));

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    bool ok = false;
    if (select(0, nullptr, &wf, nullptr, &tv) > 0) {
        int soErr = 0;
        int len = sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &len) == 0 && soErr == 0) {
            ok = true;
        }
    }
    closesocket(s);
    return ok;
}

// ============================ VDD 自定义分辨率预设(注册表) ============================
//
// 驱动的内置模式全是 16:9 横向 (3840x2160 / 2560x1440 / 1920x1080 / 1600x900 / 1280x720 ...),
// 没有竖屏模式。唯一的扩展办法: 驱动在"连接虚拟显示器之前"会去读
//     HKLM\SOFTWARE\Parsec\vdd\<0..5>   每个子键 = { width:DWORD, height:DWORD, hz:DWORD }
// 最多 5 条。写入需要管理员权限(Users 组只有 ReadKey)。
//
// 这也解释了之前"驱动假成功"的疑点: 现场残留了 0 和 1 两个**完全相同**的 2800x1840@60 预设。

static const char *VDD_PRESET_KEY = "SOFTWARE\\Parsec\\vdd";
static const int VDD_PRESET_MAX = 5;

// 定义在文件后半部(main 之前), 这里先声明以便预设命令复用同一套解析
static bool ParseSizeSpec(const std::string &spec, int *w, int *h, int *hz);

struct VddPreset
{
    int w = 0, h = 0, hz = 0;
};

struct VddPresetSlot
{
    int slot = -1;
    VddPreset p;
};

static std::vector<VddPresetSlot> VddPresetRead()
{
    std::vector<VddPresetSlot> list;
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, VDD_PRESET_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        return list;
    }
    for (int i = 0; i < VDD_PRESET_MAX; i++) {
        char sub[16];
        snprintf(sub, sizeof(sub), "%d", i);
        HKEY k = nullptr;
        if (RegOpenKeyExA(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
            continue;
        }
        auto rd = [&](const char *name) -> int {
            DWORD v = 0, sz = sizeof(DWORD), type = 0;
            if (RegQueryValueExA(k, name, nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
                type == REG_DWORD) {
                return (int)v;
            }
            return -1;
        };
        VddPresetSlot s;
        s.slot = i;
        s.p.w = rd("width");
        s.p.h = rd("height");
        s.p.hz = rd("hz");
        RegCloseKey(k);
        if (s.p.w > 0 && s.p.h > 0) {
            if (s.p.hz <= 0) {
                s.p.hz = 60;
            }
            list.push_back(s);
        }
    }
    RegCloseKey(root);
    return list;
}

static bool VddPresetWrite(int slot, const VddPreset &p, std::string *err)
{
    HKEY root = nullptr;
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, VDD_PRESET_KEY, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                             &root, nullptr);
    if (r != ERROR_SUCCESS) {
        if (r == ERROR_ACCESS_DENIED) {
            *err = "需要管理员权限 (HKLM 写入被拒绝)";
        } else {
            char b[64];
            snprintf(b, sizeof(b), "RegCreateKeyEx 失败 code=%ld", r);
            *err = b;
        }
        return false;
    }
    char sub[16];
    snprintf(sub, sizeof(sub), "%d", slot);
    HKEY k = nullptr;
    r = RegCreateKeyExA(root, sub, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
    if (r == ERROR_SUCCESS) {
        auto wr = [&](const char *name, int v) -> bool {
            DWORD d = (DWORD)v;
            return RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof(d)) == ERROR_SUCCESS;
        };
        bool ok = wr("width", p.w) && wr("height", p.h) && wr("hz", p.hz);
        RegCloseKey(k);
        if (!ok) {
            *err = "写入 width/height/hz 失败";
        }
        RegCloseKey(root);
        return ok;
    }
    RegCloseKey(root);
    if (r == ERROR_ACCESS_DENIED) {
        *err = "需要管理员权限 (HKLM 写入被拒绝)";
    } else {
        char b[64];
        snprintf(b, sizeof(b), "RegCreateKeyEx(%s) 失败 code=%ld", sub, r);
        *err = b;
    }
    return false;
}

static bool VddPresetDelete(int slot, std::string *err)
{
    HKEY root = nullptr;
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, VDD_PRESET_KEY, 0, KEY_ALL_ACCESS, &root);
    if (r != ERROR_SUCCESS) {
        *err = (r == ERROR_ACCESS_DENIED) ? "需要管理员权限 (HKLM 写入被拒绝)" : "无法打开预设键";
        return false;
    }
    char sub[16];
    snprintf(sub, sizeof(sub), "%d", slot);
    r = RegDeleteTreeA(root, sub);
    RegCloseKey(root);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        *err = (r == ERROR_ACCESS_DENIED) ? "需要管理员权限 (HKLM 写入被拒绝)" : "删除子键失败";
        return false;
    }
    return true;
}

static void VddPresetListPrint()
{
    Log("== Parsec VDD 自定义分辨率预设 (HKLM\\%s, 最多 %d 条) ==", VDD_PRESET_KEY, VDD_PRESET_MAX);
    auto list = VddPresetRead();
    if (list.empty()) {
        Log("  (无自定义预设, 驱动只用内置 16:9 横向模式)");
    }
    for (auto &s : list) {
        Log("  槽位 %d: %dx%d@%d", s.slot, s.p.w, s.p.h, s.p.hz);
    }
    // 重复检测: 驱动把预设逐条加进模式表, 完全重复的条目是已知的故障诱因
    for (size_t i = 0; i < list.size(); i++) {
        for (size_t j = i + 1; j < list.size(); j++) {
            if (list[i].p.w == list[j].p.w && list[i].p.h == list[j].p.h && list[i].p.hz == list[j].p.hz) {
                Log("  [警告] 槽位 %d 与 %d 完全重复 (%dx%d@%d) —— 建议清理", list[i].slot, list[j].slot,
                    list[i].p.w, list[i].p.h, list[i].p.hz);
            }
        }
    }
    Log("  改完之后: 驱动在每次\"插入虚拟显示器\"前才会重读该键, 所以无需重启系统;");
    Log("  若仍不生效, 再重启一次。");
}

// 清空全部槽位(0..VDD_PRESET_MAX-1)
static bool VddPresetClearAll(std::string *err)
{
    for (int i = 0; i < VDD_PRESET_MAX; i++) {
        std::string e;
        if (!VddPresetDelete(i, &e)) {
            *err = e.empty() ? "删除子键失败" : e;
            return false;
        }
    }
    return true;
}

// --vdd-preset 命令入口。cmd: list / set / add / remove / clear
static int VddPresetCommand(const std::string &cmd, const std::string &spec)
{
    if (cmd == "list") {
        VddPresetListPrint();
        return 0;
    }
    if (cmd == "clear") {
        std::string err;
        if (!VddPresetClearAll(&err)) {
            Log("[错误] 清空预设失败: %s", err.c_str());
            Log("       请以管理员身份运行, 或执行 tools\\vdd-preset.ps1 (会自动提权)");
            return 3;
        }
        Log("已清空全部自定义预设");
        VddPresetListPrint();
        return 0;
    }
    if (cmd == "remove") {
        int slot = atoi(spec.c_str());
        std::string err;
        if (!VddPresetDelete(slot, &err)) {
            Log("[错误] 删除槽位 %d 失败: %s", slot, err.c_str());
            return 3;
        }
        Log("已删除槽位 %d", slot);
        VddPresetListPrint();
        return 0;
    }
    if (cmd == "set" || cmd == "add") {
        int w = 0, h = 0, hz = 60;
        if (!ParseSizeSpec(spec, &w, &h, &hz) || w < 320 || h < 240 || w > 8192 || h > 8192) {
            Log("[错误] 分辨率格式应为 宽x高[@Hz], 例如 946x1440@60");
            return 3;
        }
        if (hz < 24 || hz > 240) {
            hz = 60;
        }
        std::string err;
        if (cmd == "set") {
            // set = 先清空再写入, 保证唯一、无重复(顺带修掉现场的重复残留)
            if (!VddPresetClearAll(&err)) {
                Log("[错误] 清空旧预设失败: %s", err.c_str());
                Log("       请以管理员身份运行, 或执行 tools\\vdd-preset.ps1 (会自动提权)");
                return 3;
            }
            VddPreset p{w, h, hz};
            if (!VddPresetWrite(0, p, &err)) {
                Log("[错误] 写入预设失败: %s", err.c_str());
                Log("       请以管理员身份运行, 或执行 tools\\vdd-preset.ps1 (会自动提权)");
                return 3;
            }
        } else {
            auto list = VddPresetRead();
            int slot = -1;
            for (int i = 0; i < VDD_PRESET_MAX && slot < 0; i++) {
                bool used = false;
                for (auto &s : list) {
                    if (s.slot == i) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    slot = i;
                }
            }
            if (slot < 0) {
                Log("[错误] 预设槽位已满 (%d 条), 先用 clear 清理", VDD_PRESET_MAX);
                return 3;
            }
            VddPreset p{w, h, hz};
            if (!VddPresetWrite(slot, p, &err)) {
                Log("[错误] 写入槽位 %d 失败: %s", slot, err.c_str());
                Log("       请以管理员身份运行, 或执行 tools\\vdd-preset.ps1 (会自动提权)");
                return 3;
            }
            Log("已写入槽位 %d", slot);
        }
        Log("已设置自定义预设 %dx%d@%d", w, h, hz);
        VddPresetListPrint();
        return 0;
    }
    Log("[错误] 未知的 --vdd-preset 子命令: %s", cmd.c_str());
    Log("       用法: --vdd-preset list|clear|remove <槽位>|add WxH[@Hz]|set WxH[@Hz]");
    return 3;
}

// ============================ Parsec VDD 虚拟显示器(扩展模式) ============================
//
// 把软件从"捕获主屏做镜像"升级为真正的扩展屏: 由本进程创建一块虚拟显示器,
// Windows 会把它当作独立显示器(可在显示设置里设为"扩展"并拖窗口过去),
// 我们再捕获它推给平板 —— 平板即成为一台真实可用的第二/第三台显示器。
//
// 关键约束(驱动行为, 必须遵守):
//   添加虚拟显示器后, 宿主必须 <100ms 周期性 ping 一次, 否则驱动会在约 1 秒内
//   自动拔掉所有已添加的显示器。所以保活线程是本模块的必要组成部分。
//   反过来这也很安全: 进程一旦退出/被强杀, 虚拟显示器会自动消失, 不会残留脏状态。

static const char *VddStatusName(parsec_vdd::DeviceStatus s)
{
    using namespace parsec_vdd;
    switch (s) {
    case DEVICE_OK:
        return "OK";
    case DEVICE_INACCESSIBLE:
        return "设备不可访问";
    case DEVICE_UNKNOWN:
        return "状态未知";
    case DEVICE_UNKNOWN_PROBLEM:
        return "未知问题";
    case DEVICE_DISABLED:
        return "设备被禁用";
    case DEVICE_DRIVER_ERROR:
        return "驱动错误";
    case DEVICE_RESTART_REQUIRED:
        return "需要重启系统";
    case DEVICE_DISABLED_SERVICE:
        return "驱动服务被禁用";
    case DEVICE_NOT_INSTALLED:
        return "驱动未安装";
    }
    return "未知状态";
}

static void ListVdd()
{
    using namespace parsec_vdd;
    Log("== Parsec VDD (虚拟显示器 / 扩展模式) ==");
    DeviceStatus st = QueryDeviceStatus(&VDD_CLASS_GUID, VDD_HARDWARE_ID);
    Log("  驱动状态: %s", VddStatusName(st));
    if (st != DEVICE_OK) {
        Log("  驱动目录: C:\\Program Files\\Parsec Virtual Display Driver");
        Log("  修复: 以管理员身份运行该目录下的 vddinstall.bat, 然后重启");
        return;
    }
    HANDLE h = OpenDeviceHandle(&VDD_ADAPTER_GUID);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        Log("  设备句柄: 打开失败");
        return;
    }
    Log("  驱动版本: 0.%d", VddVersion(h));
    CloseDeviceHandle(h);
    Log("  扩展模式: --vdd [宽x高[@Hz]]   例: --vdd 946x1440@60 (竖屏, 默认)");
    Log("  插拔守护: --watch              插入 USB 自动出屏+推流, 拔出自动销毁");
    VddPresetListPrint();
}

// 虚拟显示器的创建参数
struct VddOptions
{
    int w = 946;               // 目标分辨率(竖屏, 与平板 2800x1840 同比例, 见文档)
    int h = 1440;
    int hz = 60;
    bool fixedMode = false;    // true: 强制设为 w×h@hz; false: 沿用上次用的/Windows 记住的分辨率
    bool saveState = true;     // 移除显示器时是否把真实分辨率记进 ini(诊断模式要关掉, 免得污染)
    std::string side = "left"; // 摆位方向: left / right
};

// 上次实际使用的虚拟显示器分辨率(记在 exe 同目录的 ini 里)。
// 目的: 你在 Windows 显示设置里调好一次, 之后每次插入都复现这个值, 不会被默认值覆盖。
static std::string VddStatePath()
{
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    size_t p = dir.find_last_of("\\/");
    if (p != std::string::npos) {
        dir = dir.substr(0, p);
    }
    return dir + "\\subscreen_state.ini";
}

static bool VddStateLoad(int *w, int *h, int *hz)
{
    std::ifstream f(VddStatePath());
    if (!f) {
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("vdd=", 0) == 0) {
            return ParseSizeSpec(line.substr(4), w, h, hz) && *w >= 320 && *h >= 240;
        }
    }
    return false;
}

static void VddStateSave(int w, int h, int hz)
{
    std::ofstream f(VddStatePath(), std::ios::trunc);
    if (!f) {
        return;
    }
    f << "# SubScreenPC 状态: 虚拟显示器上次实际使用的分辨率\n";
    f << "vdd=" << w << "x" << h << "@" << hz << "\n";
}

// ================== CCD(显示配置) 拓扑激活: 解决"虚拟屏到达但不激活" ==================
//
// 现场实测(本机, 2026-09-14): Parsec VDD 用 VddAddDisplay 添加显示器后, 驱动侧
// 事件日志明确记录 "Monitor creation finished (status: STATUS_SUCCESS)", Windows 的
// PnP 里也能看到活动的 "Generic Monitor (ParsecVDA)" 设备 —— 但 DXGI EnumOutputs
// 里既没有 Parsec 适配器、也没有它的输出, 桌面始终只有原来那一块屏。
//
// 手动执行一次 DisplaySwitch /extend 之后立刻成功: 新输出出现、DuplicateOutput 和
// AcquireNextFrame 都拿到帧。结论: **本机上新建的 IddCx 虚拟显示器不会自动被扩展进
// 桌面, 必须由调用方主动提交一次显示拓扑**。被动轮询等是等不到的。
//
// 做法优先"精确激活": 用 SDC_USE_SUPPLIED_DISPLAY_CONFIG, 只把 Parsec 这块 target
// 追加进"现有活动路径 + 它"的集合。这样不会像 SDC_TOPOLOGY_EXTEND 那样把机器上
// 其他休眠的虚拟屏(GameViewer / xvdd / spacedesk)一起唤醒 —— 实测本机有这种情况。
// 精确激活失败才退化为 SDC_TOPOLOGY_EXTEND, 保证功能可用。

static bool CcdPathActive(const DISPLAYCONFIG_PATH_INFO &p)
{
    return (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
}

// 取某个 target 的识别串: 持久化设备路径 + 显示器友好名。
// Parsec 虚拟屏的特征: monitorDevicePath 含 PSCCDD0(驱动 EDID 的厂商/产品码),
// 友好名通常是 ParsecVDA / Generic Monitor (ParsecVDA)。
static std::wstring CcdTargetId(const DISPLAYCONFIG_PATH_INFO &p)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p.targetInfo.adapterId;
    tn.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) {
        return std::wstring();
    }
    std::wstring s = tn.monitorDevicePath;
    if (tn.monitorFriendlyDeviceName[0] != L'\0') {
        s += L"|";
        s += tn.monitorFriendlyDeviceName;
    }
    return s;
}

static bool CcdLooksLikeParsec(const std::wstring &id)
{
    if (id.empty()) {
        return false;
    }
    return id.find(L"PSCCDD0") != std::wstring::npos || id.find(L"ParsecVDA") != std::wstring::npos ||
           id.find(L"Parsec") != std::wstring::npos;
}

// 把 Parsec 虚拟屏激活进桌面。返回 true 表示"已经激活可用"(无论本来就激活还是本次激活的)。
// usedBlunt 回传是否退化用了整体扩展(调用方据此提示可能多出一块屏)。
static bool CcdActivateParsecTarget(bool *usedBlunt = nullptr, std::wstring *foundId = nullptr)
{
    if (usedBlunt) {
        *usedBlunt = false;
    }

    UINT32 nPath = 0, nMode = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPath, &nMode);
    if (rc != ERROR_SUCCESS) {
        Log("[拓扑] GetDisplayConfigBufferSizes 失败 (rc=%ld)", rc);
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode ? nMode : 1);
    rc = QueryDisplayConfig(QDC_ALL_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        Log("[拓扑] QueryDisplayConfig 失败 (rc=%ld)", rc);
        return false;
    }
    paths.resize(nPath);
    modes.resize(nMode);

    int target = -1;
    for (int i = 0; i < (int)paths.size(); i++) {
        if (!paths[i].targetInfo.targetAvailable) {
            continue;
        }
        std::wstring id = CcdTargetId(paths[i]);
        if (!CcdLooksLikeParsec(id)) {
            continue;
        }
        if (foundId) {
            *foundId = id;
        }
        if (CcdPathActive(paths[i])) {
            Log("[拓扑] Parsec 虚拟屏已经是活动输出, 无需提交");
            return true;
        }
        target = i;
        break;
    }
    if (target < 0) {
        Log("[拓扑] 在显示配置里没找到 Parsec 虚拟屏 target(驱动可能还没把它登记进来)");
        return false;
    }

    // 精确激活: 现有活动路径原样保留 + 追加这一块 target, 模式交给系统挑
    std::vector<DISPLAYCONFIG_PATH_INFO> want;
    for (auto &p : paths) {
        if (CcdPathActive(p)) {
            want.push_back(p);
        }
    }
    DISPLAYCONFIG_PATH_INFO np = paths[target];
    np.flags = DISPLAYCONFIG_PATH_ACTIVE;
    np.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    np.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    want.push_back(np);

    rc = SetDisplayConfig((UINT32)want.size(), want.data(), (UINT32)modes.size(), modes.data(),
                          SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_APPLY);
    if (rc == ERROR_SUCCESS) {
        Log("[拓扑] 已精确激活 Parsec 虚拟屏 (提交 %zu 条路径)", want.size());
        return true;
    }
    Log("[拓扑] 精确激活失败 (rc=%ld), 退化为整体扩展", rc);

    rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
    if (rc == ERROR_SUCCESS) {
        if (usedBlunt) {
            *usedBlunt = true;
        }
        Log("[拓扑] 已用 SDC_TOPOLOGY_EXTEND 扩展桌面(可能连带唤醒其他休眠虚拟屏)");
        return true;
    }
    Log("[拓扑] 扩展桌面同样失败 (rc=%ld)", rc);
    return false;
}

// ================== 通用: 按关键字精确开关某块显示器 (--topology / GUI 用) ==================
//
// 用户场景: 平时桌面只有外接屏("仅显示屏"); 要用平板时, 把**笔记本内屏**也点亮,
// 组成"外接屏 + 内屏"的双屏扩展, 然后只捕获内屏镜像到平板。
//
// 为什么不用 DisplaySwitch /extend:
//   它是"粗暴整体扩展", 会把机器上所有 available 的显示输出一起激活 ——
//   本机装着 4 套虚拟显示驱动(GameViewer / xvdd / spacedesk), 实测会多唤醒出
//   一块 "\\.\DISPLAY9" 之类的幽灵屏。精确法只动目标那一块。
//
// 采集: QDC_ALL_PATHS 能看到"可用但当前未激活"的显示器, 这是精确开关的前提。

struct CcdTargetInfo
{
    int index = -1;      // 在 QDC_ALL_PATHS 里的下标
    bool active = false; // 当前是否已接入桌面
    std::wstring name;   // 显示器友好名(EDID 型号, 如 NE160QDM-NZC)
    std::wstring pathId; // 持久化设备路径(如 \\?\DISPLAY#BOE0CFB#...)
    LUID adapterId{};    // 这条路径挂在哪张显卡上
    std::wstring gpu;    // 显卡名(如 NVIDIA ... / Intel ...)
};

// LUID -> 显卡名。同一台笔记本的显示器可能同时"够得着"核显和独显(多接口/多路径),
// 排查"外接屏为什么走了核显"必须有这个映射。
static std::wstring CcdGpuNameForLUID(const LUID &luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return std::wstring();
    }
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d)) && d.AdapterLuid.HighPart == luid.HighPart &&
            d.AdapterLuid.LowPart == luid.LowPart) {
            return d.Description;
        }
    }
    return std::wstring();
}

static bool CcdSnapTargets(std::vector<DISPLAYCONFIG_PATH_INFO> *paths,
                           std::vector<DISPLAYCONFIG_MODE_INFO> *modes)
{
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPath, &nMode) != ERROR_SUCCESS) {
        return false;
    }
    paths->assign(nPath, DISPLAYCONFIG_PATH_INFO{});
    modes->assign(nMode ? nMode : 1, DISPLAYCONFIG_MODE_INFO{});
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPath, paths->data(), &nMode, modes->data(), nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    paths->resize(nPath);
    modes->resize(nMode);
    return true;
}

static std::vector<CcdTargetInfo> CcdEnumTargets()
{
    std::vector<CcdTargetInfo> out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!CcdSnapTargets(&paths, &modes)) {
        return out;
    }
    for (int i = 0; i < (int)paths.size(); i++) {
        if (!paths[i].targetInfo.targetAvailable) {
            continue; // 被禁用/拔掉的, 动不了
        }
        std::wstring id = CcdTargetId(paths[i]);
        if (id.empty()) {
            continue;
        }
        CcdTargetInfo t;
        t.index = i;
        t.active = CcdPathActive(paths[i]);
        t.adapterId = paths[i].targetInfo.adapterId;
        t.gpu = CcdGpuNameForLUID(t.adapterId);
        size_t bar = id.find(L'|');
        t.pathId = (bar == std::wstring::npos) ? id : id.substr(0, bar);
        t.name = (bar == std::wstring::npos) ? L"" : id.substr(bar + 1);
        out.push_back(t);
    }
    return out;
}

// 把匹配 key(大小写不敏感的子串, 比对友好名+设备路径) 的显示器接入(active=true)
// 或移出(false)桌面。返回 false 时 err 里是原因。
static bool CcdSetTargetActive(const std::wstring &key, bool active, std::string *err)
{
    if (key.empty()) {
        *err = "关键字为空";
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!CcdSnapTargets(&paths, &modes)) {
        *err = "QueryDisplayConfig 失败";
        return false;
    }

    // ⚠️ 同一块物理显示器在 QDC_ALL_PATHS 里往往有多条路径(历史会话的残留), 且各次查询的
    // 顺序不稳定。只看"第一条匹配"会踩坑: 关屏时匹配到的可能是一条未激活的副本, 误判成
    // "已经是目标状态"而直接返回, 真正活动的那条根本没动 —— 表现就是"停止后内屏还亮着"。
    // 所以这里必须把所有匹配都收集起来, 按"有没有活动副本"来判断。
    std::vector<int> matches;
    for (int i = 0; i < (int)paths.size(); i++) {
        if (!paths[i].targetInfo.targetAvailable) {
            continue;
        }
        std::wstring id = CcdTargetId(paths[i]);
        if (!id.empty() && WStrContainsNoCase(id, key)) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        *err = "显示配置里没有匹配的显示器(它可能真的不存在或被禁用)";
        return false;
    }
    bool anyActive = false;
    for (int i : matches) {
        if (CcdPathActive(paths[i])) {
            anyActive = true;
        }
    }
    if (active && anyActive) {
        return true; // 已有活动副本, 无需再接
    }
    if (!active && !anyActive) {
        return true; // 本来就没有活动副本, 无需再移
    }

    // 目标集合 = "其他保持原状的已激活路径" + 匹配者的期望状态
    std::vector<DISPLAYCONFIG_PATH_INFO> want;
    for (int i = 0; i < (int)paths.size(); i++) {
        if (CcdPathActive(paths[i])) {
            bool isMatch = false;
            for (int m : matches) {
                if (m == i) {
                    isMatch = true;
                }
            }
            if (!isMatch) {
                want.push_back(paths[i]);
            }
        }
    }
    if (active) {
        DISPLAYCONFIG_PATH_INFO np = paths[matches[0]];
        np.flags = DISPLAYCONFIG_PATH_ACTIVE;
        // 让系统自己挑模式。注意: 这不会保留"显示方向"(实测会被重置回横向),
        // 方向由调用方在接入后用 SetPanelOrientation 显式设置。
        np.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        np.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        want.push_back(np);
    }
    if (want.empty()) {
        *err = "拒绝提交空拓扑(那会把所有屏幕都关掉)";
        return false;
    }

    LONG rc = SetDisplayConfig((UINT32)want.size(), want.data(), (UINT32)modes.size(), modes.data(),
                               SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_APPLY);
    if (rc != ERROR_SUCCESS) {
        char b[96];
        snprintf(b, sizeof(b), "SetDisplayConfig 失败 rc=%ld", rc);
        *err = b;
        return false;
    }
    return true;
}

// 把匹配 key 的显示器切到 gpuKey 那张显卡的路径上(例如让外接屏走独显直连)。
// 前提: 该显示器在目标显卡上真的有路径 —— 没有就说明线缆插在另一张卡的口上(或独显直连未开),
// 软件无法把信号"渡"过去。换路径时会保留当前的显示方向(外接屏是面板倒装 ROTATE180, 不能丢)。
static bool CcdRouteTarget(const std::wstring &key, const std::wstring &gpuKey, std::string *err)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!CcdSnapTargets(&paths, &modes)) {
        *err = "QueryDisplayConfig 失败";
        return false;
    }
    std::vector<int> matches;
    for (int i = 0; i < (int)paths.size(); i++) {
        if (!paths[i].targetInfo.targetAvailable) {
            continue;
        }
        std::wstring id = CcdTargetId(paths[i]);
        if (!id.empty() && WStrContainsNoCase(id, key)) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        *err = "没有匹配的显示器";
        return false;
    }
    int activeIdx = -1;
    for (int i : matches) {
        if (CcdPathActive(paths[i])) {
            activeIdx = i;
        }
    }
    int dst = -1;
    std::wstring dstGpu;
    for (int i : matches) {
        std::wstring gpu = CcdGpuNameForLUID(paths[i].targetInfo.adapterId);
        if (!gpu.empty() && WStrContainsNoCase(gpu, gpuKey)) {
            dst = i;
            dstGpu = gpu;
            break;
        }
    }
    if (dst < 0) {
        *err = "该显示器在目标显卡上没有可用路径(线缆可能插在核显的口上, 或 BIOS/奥创中心里独显直连未开)";
        return false;
    }
    if (dst == activeIdx) {
        return true; // 已经在目标卡上
    }
    if (activeIdx < 0) {
        *err = "该显示器当前未接入桌面, 请先 --topology on";
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> want;
    for (int i = 0; i < (int)paths.size(); i++) {
        if (CcdPathActive(paths[i]) && i != activeIdx) {
            want.push_back(paths[i]);
        }
    }
    DISPLAYCONFIG_PATH_INFO np = paths[dst];
    np.flags = DISPLAYCONFIG_PATH_ACTIVE;
    np.targetInfo.rotation = paths[activeIdx].targetInfo.rotation; // 保住 ROTATE180
    np.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    np.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    want.push_back(np);
    if (want.empty()) {
        *err = "拒绝提交空拓扑";
        return false;
    }
    LONG rc = SetDisplayConfig((UINT32)want.size(), want.data(), (UINT32)modes.size(), modes.data(),
                               SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_APPLY);
    if (rc != ERROR_SUCCESS) {
        char b[96];
        snprintf(b, sizeof(b), "SetDisplayConfig 失败 rc=%ld", rc);
        *err = b;
        return false;
    }
    char g[128]{};
    WideCharToMultiByte(CP_UTF8, 0, dstGpu.c_str(), -1, g, sizeof(g) - 1, nullptr, nullptr);
    Log("[路由] 显示器已切到: %s", g);
    return true;
}

// 把匹配 key 的活动显示器转到指定方向(DMDO_90 = Windows 显示设置里的"纵向")。
// 用 ChangeDisplaySettingsExW: 改单屏方向的标准 API, 不需要管理员, 且设置会被记住。
// 注意: 接入显示器时用 MODE_IDX_INVALID 让系统挑模式, 实测会把方向重置回横向 ——
// 所以接入后必须用这里显式把方向调回来(用户要竖屏 90°)。
static bool SetPanelOrientation(const std::wstring &key, DWORD dmdo, std::string *err)
{
    std::wstring dev;
    for (auto &kv : CcdActiveDisplayNames()) {
        if (WStrContainsNoCase(kv.second, key)) {
            dev = kv.first;
            break;
        }
    }
    if (dev.empty()) {
        *err = "活动拓扑里没有匹配的显示器";
        return false;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        *err = "EnumDisplaySettingsW 失败";
        return false;
    }
    if (dm.dmDisplayOrientation == dmdo) {
        return true; // 已经是目标方向
    }
    bool wasPortrait = dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270;
    bool nowPortrait = dmdo == DMDO_90 || dmdo == DMDO_270;
    if (wasPortrait != nowPortrait) {
        std::swap(dm.dmPelsWidth, dm.dmPelsHeight); // 方向族变了, 桌面宽高要对调
    }
    dm.dmDisplayOrientation = dmdo;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG rc = ChangeDisplaySettingsExW(dev.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        char b[96];
        snprintf(b, sizeof(b), "ChangeDisplaySettingsExW rc=%ld", rc);
        *err = b;
        return false;
    }
    return true;
}

// --topology list | on <关键字> | off <关键字> | gpu <显示器> <显卡>
static int TopologyCommand(const std::string &sub, const std::string &keyUtf8)
{
    if (sub == "list") {
        Log("== 显示配置里的所有可用输出 (含当前未激活的) ==");
        auto ts = CcdEnumTargets();
        if (ts.empty()) {
            Log("  (读不到显示配置)");
            return 3;
        }
        for (auto &t : ts) {
            char name[256]{}, gpu[128]{};
            WideCharToMultiByte(CP_UTF8, 0, t.name.c_str(), -1, name, sizeof(name) - 1, nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, t.gpu.c_str(), -1, gpu, sizeof(gpu) - 1, nullptr, nullptr);
            Log("  [%d] %-8s [%s] %s", t.index, t.active ? "已接入" : "未接入", gpu, name);
        }
        Log("  提示: 同一块屏可能出现多行 = 它有多条路径(不同接口/显卡)。");
        Log("        要把某块屏切到指定显卡: --topology gpu <显示器关键字> <显卡关键字>");
        return 0;
    }
    if (sub == "gpu") {
        // --topology gpu <显示器关键字> <显卡关键字>
        std::string mon, gpu;
        // keyUtf8 里装的是"显示器关键字", 显卡关键字从第二个参数来 —— 但命令行只传了一个,
        // 所以这里约定: gpu 子命令时 topoKey = "显示器|显卡" 的形式由解析层拆好; 若没有 |, 视为空。
        size_t bar = keyUtf8.find('|');
        if (bar == std::string::npos) {
            Log("用法: --topology gpu <显示器关键字>|<显卡关键字>   例: --topology gpu G52E|NVIDIA");
            return 3;
        }
        mon = keyUtf8.substr(0, bar);
        gpu = keyUtf8.substr(bar + 1);
        std::string err;
        if (!CcdRouteTarget(Utf8ToWide(mon), Utf8ToWide(gpu), &err)) {
            Log("[路由] 失败: %s", err.c_str());
            return 3;
        }
        return 0;
    }
    if (sub == "on" || sub == "off") {
        std::wstring key = Utf8ToWide(keyUtf8);
        std::string err;
        bool ok = CcdSetTargetActive(key, sub == "on", &err);
        if (!ok) {
            Log("[拓扑] %s 失败: %s", sub == "on" ? "接入" : "移出", err.c_str());
            return 3;
        }
        Log("[拓扑] 已%s匹配 \"%s\" 的显示器", sub == "on" ? "接入" : "移出", keyUtf8.c_str());
        return 0;
    }
    Log("用法: --topology list | on <关键字> | off <关键字>");
    Log("      关键字按显示器 EDID 型号匹配, 例: --topology on NE160QDM");
    return 3;
}

class VddDisplay
{
public:
    ~VddDisplay() { Cleanup(); }

    // 创建一块虚拟显示器, 按 opt 设置分辨率与摆位(只在必要时动, 见 ApplyDisplay),
    // 成功时返回它在 DXGI 里的输出索引(*outDxgiIndex)。
    bool Create(const VddOptions &opt, int *outDxgiIndex, bool keepAlive = true)
    {
        using namespace parsec_vdd;
        opt_ = opt;

        DeviceStatus st = QueryDeviceStatus(&VDD_CLASS_GUID, VDD_HARDWARE_ID);
        if (st != DEVICE_OK) {
            Log("[错误] Parsec VDD 驱动不可用: %s", VddStatusName(st));
            Log("       修复: 以管理员身份运行 \"C:\\Program Files\\Parsec Virtual Display Driver\\vddinstall.bat\"");
            return false;
        }
        vdd_ = OpenDeviceHandle(&VDD_ADAPTER_GUID);
        if (vdd_ == nullptr || vdd_ == INVALID_HANDLE_VALUE) {
            vdd_ = INVALID_HANDLE_VALUE;
            Log("[错误] 打开 Parsec VDD 设备句柄失败 (驱动在, 但设备接口不可用)");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(io_);
            Log("Parsec VDD 就绪, 驱动版本 0.%d", parsec_vdd::VddVersion(vdd_));
        }

        // 记录添加前的输出集合, 之后用差集认出新显示器
        std::vector<std::wstring> before;
        for (auto &e : EnumOutputs()) {
            before.push_back(e.deviceName);
        }

        // 保活线程: 驱动要求 <100ms ping 一次, 否则已添加的显示器约 1 秒后被自动拔掉。
        // 所有 ioctl 都用 io_ 串行化 —— core API 没有声明线程安全, 不能让 ping 和 ADD 并发。
        // keepAlive=false 仅用于诊断(禁掉 ping 观察 ADD 本身是否生效)。
        if (!keepAlive) {
            Log("[诊断] 已禁用保活 ping");
        } else {
            pingRun_ = true;
            ping_ = std::thread([this] {
                while (pingRun_.load()) {
                    std::lock_guard<std::mutex> lk(io_);
                    parsec_vdd::VddUpdate(vdd_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        }

        // 添加虚拟显示器。实测驱动会"接受请求但没把显示器挂上桌面"(返回有效 index 却
        // 什么都没发生), 所以移除后耐心重试几次 —— 同时也覆盖了系统枚举较慢的情况。
        std::wstring devName;
        int newIndex = -1;
        const int kMaxAttempt = 3;
        for (int attempt = 1; attempt <= kMaxAttempt && newIndex < 0 && !g_exit.load(); attempt++) {
            int idx;
            {
                std::lock_guard<std::mutex> lk(io_);
                idx = parsec_vdd::VddAddDisplay(vdd_);
            }
            if (idx < 0) {
                Log("[错误] 添加虚拟显示器失败 (驱动拒绝 ADD, 第 %d 次)", attempt);
                break;
            }
            idx_ = idx;
            added_ = true;
            Log("已添加虚拟显示器 (VDD index=%d), 等待系统枚举...", idx_);

            // 第一段: 短暂被动等待 —— 覆盖"系统会自己把新显示器扩展进桌面"的机器,
            // 那种情况 1~2 秒就能看到输出, 不需要我们插手。
            newIndex = WaitForNewOutput(before, 2, &devName);

            // 第二段: 本机实测**不会**自动扩展 —— 驱动建屏成功、PnP 里也有活动的
            // ParsecVDA 显示器设备, 但桌面拓扑一直不认它, 被动等永远等不到。必须主动
            // 提交一次显示配置(见文件上方 CcdActivateParsecTarget 的说明)。
            if (newIndex < 0) {
                Log("[提示] 新显示器已到达但未被系统激活, 主动提交显示拓扑...");
                bool blunt = false;
                if (CcdActivateParsecTarget(&blunt) && blunt) {
                    Log("[提示] 走了整体扩展, 桌面上可能多出其他虚拟显示驱动的屏(与本程序无关)");
                }
                newIndex = WaitForNewOutput(before, 8, &devName);
            }

            if (newIndex >= 0) {
                break;
            }
            if (attempt == 1 || attempt == kMaxAttempt) {
                DumpOutputs("添加后 10 秒内没有新输出挂上桌面");
                DumpAdapters();
            }
            {
                std::lock_guard<std::mutex> lk(io_);
                parsec_vdd::VddRemoveDisplay(vdd_, idx_);
            }
            added_ = false;
            if (attempt < kMaxAttempt) {
                Log("[提示] 驱动未把显示器挂上桌面, %d ms 后重试 (%d/%d)", 1500 * attempt, attempt + 1,
                    kMaxAttempt);
                SleepInterruptible(1500 * attempt);
            }
        }
        if (newIndex < 0) {
            Log("[错误] 虚拟显示器创建失败: 驱动接受了 ADD, 但系统始终没产生新的桌面输出");
            Log("       本程序已经主动提交过显示拓扑(CCD), 仍然失败, 按下面顺序查:");
            Log("       1) 驱动预设里有没有完全重复的条目: --vdd-preset list");
            Log("       2) 显示配置里有没有 Parsec 的 target(见上方 [拓扑] 日志):");
            Log("          找不到 target 说明驱动没把显示器登记进系统, 重启 Parsec 适配器/驱动");
            Log("       3) 本机同时装着 GameViewer / xvdd / spacedesk 等虚拟显示驱动, 可能互相干扰");
            Log("       4) 预设有问题就清理(需管理员): --vdd-preset clear");
            return false;
        }
        Log("虚拟显示器已就位: %ls (DXGI #%d)", devName.c_str(), newIndex);

        // 按需设分辨率/摆位(默认不改分辨率, 只在重叠时摆到一侧)
        ApplyDisplay(devName, opt);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // 分辨率变更后重新枚举, 拿最终索引(索引可能因重排而变化)
        int finalIndex = -1;
        for (auto &e : EnumOutputs()) {
            if (e.deviceName == devName) {
                finalIndex = e.index;
                break;
            }
        }
        if (finalIndex < 0) {
            Log("[错误] 设置分辨率后虚拟显示器消失");
            return false;
        }
        *outDxgiIndex = finalIndex;
        devName_ = devName;
        Log("虚拟显示器就绪: %ls, 现在把窗口拖到它上面即可显示到平板", devName.c_str());
        return true;
    }

    void Cleanup()
    {
        if (pingRun_.exchange(false)) {
            if (ping_.joinable()) {
                ping_.join();
            }
        }
        // 移除之前把"真实生效的分辨率"记下来 —— 包括用户在 Windows 显示设置里手动改过的值。
        // 下次插入就复现它, 既尊重用户的选择, 也避免我们每次都用默认值覆盖。
        if (added_ && opt_.saveState && !devName_.empty()) {
            DEVMODEW cur{};
            cur.dmSize = sizeof(DEVMODEW);
            if (EnumDisplaySettingsExW(devName_.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0) &&
                cur.dmPelsWidth > 0 && cur.dmPelsHeight > 0) {
                VddStateSave((int)cur.dmPelsWidth, (int)cur.dmPelsHeight,
                             (int)(cur.dmDisplayFrequency ? cur.dmDisplayFrequency : 60));
            }
        }
        if (added_ && vdd_ != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lk(io_);
            parsec_vdd::VddRemoveDisplay(vdd_, idx_);
            Log("已移除虚拟显示器 (VDD index=%d)", idx_);
            added_ = false;
        }
        if (vdd_ != INVALID_HANDLE_VALUE) {
            parsec_vdd::CloseDeviceHandle(vdd_);
            vdd_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    static bool Contains(const std::vector<std::wstring> &v, const std::wstring &s)
    {
        for (auto &x : v) {
            if (x == s) {
                return true;
            }
        }
        return false;
    }

    // 轮询等待出现一个不在 before 里、且已挂到桌面的新输出
    static int WaitForNewOutput(const std::vector<std::wstring> &before, int seconds, std::wstring *outName)
    {
        for (int i = 0; i < seconds * 10 && !g_exit.load(); i++) {
            for (auto &e : EnumOutputs()) {
                if (!Contains(before, e.deviceName)) {
                    *outName = e.deviceName;
                    return e.index;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return -1;
    }

    // 按需设置虚拟显示器的分辨率与摆位。
    //
    // 设计原则(用户明确要求"大小不用预设, 在 Windows 显示设置里自己调"):
    //   * 分辨率: 默认**不动**。优先复现"上次实际用过的"值(记在 subscreen_state.ini,
    //     由 Cleanup 在移除显示器前抓取真实值写入), 所以你在 Windows 显示设置里调的
    //     任何值都会被沿用, 不会被我们覆盖回默认值。仅 --vdd-fixed 才强制覆盖。
    //   * 摆位: 只在它与已有屏幕**重叠**时才移到 --side 指定的一侧; 用户手动拖开之后
    //     不会被我们拽回来。
    //
    // 选模式的回退策略: 先精确匹配; 匹配不到则退到"长宽比最接近"的模式, 再在同比例
    // 候选里取面积最接近的 —— 竖屏目标不会被退成 16:9 横向而被拉扁。
    static void ApplyDisplay(const std::wstring &devName, const VddOptions &opt)
    {
        struct Mode
        {
            int w, h, hz;
        };
        std::vector<Mode> modes;
        DEVMODEW dm{};
        dm.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplaySettingsExW(devName.c_str(), i, &dm, 0); i++) {
            Mode m{(int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency};
            modes.push_back(m);
        }

        // ---- 1) 决定目标分辨率 ----
        int wantW = opt.w, wantH = opt.h, wantHz = opt.hz;
        bool applySize = false;
        bool fromState = false;
        if (opt.fixedMode) {
            applySize = true;
        } else {
            int sw = 0, sh = 0, shz = 0;
            if (VddStateLoad(&sw, &sh, &shz)) {
                wantW = sw;
                wantH = sh;
                wantHz = shz;
                applySize = true;
                fromState = true;
                Log("沿用上次使用过的虚拟显示器分辨率 %d×%d@%d", wantW, wantH, wantHz);
            } else {
                // 首次插入还没有历史值: 只有驱动确实提供该模式时才设, 否则交给用户在
                // Windows 显示设置里自己定(避免我们把错的默认值写死)。
                for (auto &m : modes) {
                    if (m.w == wantW && m.h == wantH) {
                        applySize = true;
                        break;
                    }
                }
                if (applySize) {
                    Log("首次插入: 使用默认竖屏 %d×%d@%d (之后你在显示设置里的调整会被记住)", wantW, wantH,
                        wantHz);
                } else {
                    Log("首次插入: 虚拟显示器没有 %d×%d 模式, 保持系统默认分辨率不动", wantW, wantH);
                    Log("       想要精确的竖屏尺寸, 先把它加进驱动预设(需管理员):");
                    Log("       subscreen_sender.exe --vdd-preset set %dx%d@%d", wantW, wantH,
                        wantHz > 0 ? wantHz : 60);
                    // 把驱动实际提供的模式打出来 —— 用来判断"预设到底有没有被驱动读进去":
                    // 列表里没有目标尺寸就说明驱动没读到预设(改完预设要重载适配器), 而不是我们没设。
                    std::vector<std::pair<int, int>> uniq;
                    for (auto &m : modes) {
                        std::pair<int, int> p(m.w, m.h);
                        bool dup = false;
                        for (auto &q : uniq) {
                            if (q == p) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            uniq.push_back(p);
                        }
                        if (uniq.size() >= 20) {
                            break;
                        }
                    }
                    std::string list;
                    char mbuf[32];
                    for (auto &q : uniq) {
                        snprintf(mbuf, sizeof(mbuf), "%d×%d ", q.first, q.second);
                        list += mbuf;
                    }
                    Log("       驱动实际提供的分辨率(%zu 种): %s", uniq.size(), list.c_str());
                }
            }
        }

        // ---- 2) 计算摆位 ----
        RECT self{};
        bool haveSelf = false;
        std::vector<RECT> others;
        bool haveEdge = false, havePrimary = false, haveAny = false;
        LONG minLeft = 0, maxRight = 0, primaryTop = 0, firstTop = 0;
        for (auto &e : EnumOutputs()) {
            const RECT &r = e.desc.DesktopCoordinates;
            if (e.deviceName == devName) {
                self = r;
                haveSelf = true;
                continue;
            }
            others.push_back(r);
            if (!haveAny) {
                firstTop = r.top;
                haveAny = true;
            }
            if (!haveEdge || r.left < minLeft) {
                minLeft = r.left;
            }
            if (!haveEdge || r.right > maxRight) {
                maxRight = r.right;
            }
            haveEdge = true;
            if (r.left == 0 && r.top == 0) {
                primaryTop = r.top;
                havePrimary = true;
            }
        }
        bool overlaps = false;
        if (haveSelf) {
            for (auto &r : others) {
                if (self.left < r.right && self.right > r.left && self.top < r.bottom && self.bottom > r.top) {
                    overlaps = true;
                    break;
                }
            }
        }
        LONG top = havePrimary ? primaryTop : (haveAny ? firstTop : 0);

        // 左侧定位时用"本次生效的宽度"算 x, 否则会算错
        int effW = applySize ? wantW : (haveSelf ? (int)(self.right - self.left) : opt.w);
        LONG x = 0;
        if (opt.side == "right") {
            x = haveEdge ? maxRight : 0;
        } else {
            x = haveEdge ? (minLeft - (LONG)effW) : 0;
        }

        // ---- 3) 决定要动哪些字段 ----
        DWORD fields = 0;
        bool movePos = overlaps || opt.fixedMode || !haveSelf;
        if (applySize) {
            movePos = true; // 改分辨率时一并给位置, 否则系统可能把新尺寸摆到重叠处
        }
        if (!applySize && !movePos) {
            if (haveSelf) {
                Log("虚拟显示器: 沿用 Windows 记住的设置 %d×%d @(%ld,%ld), 未改动",
                    (int)(self.right - self.left), (int)(self.bottom - self.top), self.left, self.top);
            }
            return;
        }
        if (applySize) {
            fields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        }
        if (movePos) {
            fields |= DM_POSITION;
        }

        // ---- 4) 需要改分辨率时先挑模式 ----
        int pickW = 0, pickH = 0, pickHz = 0;
        if (applySize) {
            if (modes.empty()) {
                Log("[警告] 未枚举到虚拟显示器可用模式, 沿用系统默认分辨率");
                fields &= ~(DWORD)(DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY);
                if (!fields) {
                    return;
                }
            } else {
                int pick = -1;
                for (size_t i = 0; i < modes.size(); i++) {
                    if (modes[i].w != wantW || modes[i].h != wantH) {
                        continue;
                    }
                    if (pick < 0) {
                        pick = (int)i;
                    }
                    if (wantHz > 0 && modes[i].hz == wantHz) {
                        pick = (int)i;
                        break;
                    }
                    if (wantHz <= 0 && modes[i].hz > modes[pick].hz) {
                        pick = (int)i;
                    }
                }
                bool exact = (pick >= 0);
                if (!exact) {
                    const double wantRatio = (double)wantW / (double)wantH;
                    const long long wantArea = (long long)wantW * wantH;
                    double bestRatioDelta = -1.0;
                    for (auto &m : modes) {
                        double d = (double)m.w / (double)m.h - wantRatio;
                        if (d < 0) {
                            d = -d;
                        }
                        if (bestRatioDelta < 0 || d < bestRatioDelta) {
                            bestRatioDelta = d;
                        }
                    }
                    long long bestAreaDelta = -1;
                    for (size_t i = 0; i < modes.size(); i++) {
                        double d = (double)modes[i].w / (double)modes[i].h - wantRatio;
                        if (d < 0) {
                            d = -d;
                        }
                        if (d > bestRatioDelta + 0.02) {
                            continue; // 只比"比例最接近"的那一档
                        }
                        long long a = (long long)modes[i].w * modes[i].h - wantArea;
                        if (a < 0) {
                            a = -a;
                        }
                        if (bestAreaDelta < 0 || a < bestAreaDelta) {
                            bestAreaDelta = a;
                            pick = (int)i;
                        }
                    }
                    Log("[警告] 虚拟显示器没有 %d×%d 模式%s, 退用比例最接近的 %d×%d@%dHz", wantW, wantH,
                        fromState ? "(上次用的, 可能已被移除)" : "", modes[pick].w, modes[pick].h, modes[pick].hz);
                    Log("       竖屏尺寸属于非标准分辨率, 需要先加进驱动预设(需管理员):");
                    Log("       subscreen_sender.exe --vdd-preset set %dx%d@%d", wantW, wantH,
                        wantHz > 0 ? wantHz : 60);
                    // 把可用分辨率打出来供调参
                    std::vector<std::pair<int, int>> uniq;
                    for (auto &m : modes) {
                        std::pair<int, int> p(m.w, m.h);
                        bool dup = false;
                        for (auto &q : uniq) {
                            if (q == p) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            uniq.push_back(p);
                        }
                        if (uniq.size() >= 16) {
                            break;
                        }
                    }
                    std::string list;
                    char buf[32];
                    for (auto &q : uniq) {
                        snprintf(buf, sizeof(buf), "%d×%d ", q.first, q.second);
                        list += buf;
                    }
                    Log("       可用分辨率: %s", list.c_str());
                }
                pickW = modes[pick].w;
                pickH = modes[pick].h;
                pickHz = modes[pick].hz;
            }
        }

        // ---- 5) 应用 ----
        DEVMODEW out{};
        out.dmSize = sizeof(DEVMODEW);
        if (!EnumDisplaySettingsExW(devName.c_str(), ENUM_CURRENT_SETTINGS, &out, 0)) {
            out.dmSize = sizeof(DEVMODEW);
        }
        if (applySize && pickW > 0) {
            out.dmPelsWidth = (DWORD)pickW;
            out.dmPelsHeight = (DWORD)pickH;
            out.dmDisplayFrequency = (DWORD)pickHz;
        }
        if (movePos) {
            out.dmPosition.x = x;
            out.dmPosition.y = top;
        }
        out.dmFields = fields | DM_BITSPERPEL;

        LONG r = ChangeDisplaySettingsExW(devName.c_str(), &out, nullptr, CDS_UPDATEREGISTRY, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            // 位置常是失败原因(例如与已有屏幕重叠), 退一步: 只改分辨率、不动位置
            out.dmFields = (fields & ~(DWORD)DM_POSITION) | DM_BITSPERPEL;
            LONG r2 = ChangeDisplaySettingsExW(devName.c_str(), &out, nullptr, CDS_UPDATEREGISTRY, nullptr);
            Log("[警告] 设置虚拟显示器失败(code=%ld), 改为只设分辨率重试: %ld", r, r2);
            if (r2 != DISP_CHANGE_SUCCESSFUL) {
                Log("       保持当前设置不变");
                return;
            }
        }
        Log("虚拟显示器已设置: %d×%d@%dHz @(%ld,%ld) [%s]", applySize ? pickW : (int)out.dmPelsWidth,
            applySize ? pickH : (int)out.dmPelsHeight, (int)out.dmDisplayFrequency, out.dmPosition.x,
            out.dmPosition.y, opt.side == "right" ? "右侧" : "左侧");
    }

    HANDLE vdd_ = INVALID_HANDLE_VALUE;
    int idx_ = -1;
    bool added_ = false;
    std::wstring devName_; // 已创建的虚拟显示器设备名, 用于移除前抓取真实分辨率
    VddOptions opt_;       // 本次创建参数(供 Cleanup 判断是否记录状态)
    std::mutex io_; // 串行化所有 VDD ioctl(ping 线程与 ADD/REMOVE 不能并发)
    std::atomic<bool> pingRun_{false};
    std::thread ping_;
};

// 诊断用: 创建虚拟显示器后连续观察桌面拓扑变化, 并实测能否建立桌面复制。
// 用来区分"拓扑一直在抖"和"拓扑稳定但复制拿不到"这两类故障。
static int VddProbe(bool keepAlive = true)
{
    VddDisplay vdd;
    VddOptions opt;
    opt.w = 1920;
    opt.h = 1080;
    opt.hz = 60;
    opt.fixedMode = true; // 诊断时强制一个已知模式, 便于对比
    opt.side = "right";
    opt.saveState = false; // 别把诊断用的分辨率写进状态文件
    int idx = -1;
    if (!vdd.Create(opt, &idx, keepAlive)) {
        return 3;
    }

    Log("---- 拓扑观察 (每 400ms 一次) ----");
    DumpAdapters();
    for (int t = 0; t < 12 && !g_exit.load(); t++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "probe %d", t);
        DumpOutputs(tag);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    // 实测: 对每个已挂桌面的输出尝试建立桌面复制(立即释放), 报告 HRESULT
    Log("---- 桌面复制可用性实测 ----");
    for (auto &e : EnumOutputs()) {
        if (e.output == nullptr) {
            continue;
        }
        ComPtr<ID3D11Device> dev;
        HRESULT hr = D3D11CreateDevice(e.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        if (FAILED(hr)) {
            Log("      #%d %ls: D3D11CreateDevice 失败 0x%08lX", e.index, e.deviceName.c_str(), hr);
            continue;
        }
        ComPtr<IDXGIOutputDuplication> dup;
        HRESULT hr2 = e.output->DuplicateOutput(dev.Get(), &dup);
        Log("      #%d %ls: DuplicateOutput %s (0x%08lX)", e.index, e.deviceName.c_str(),
            SUCCEEDED(hr2) ? "成功" : "失败", hr2);
        if (SUCCEEDED(hr2)) {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr3 = dup->AcquireNextFrame(2000, &fi, &res);
            Log("           AcquireNextFrame: %s (0x%08lX)%s", SUCCEEDED(hr3) ? "拿到帧" : "失败", hr3,
                hr3 == DXGI_ERROR_WAIT_TIMEOUT ? " [超时=桌面静止, 属正常]" : "");
            if (SUCCEEDED(hr3)) {
                dup->ReleaseFrame();
            }
        }
    }
    return 0;
}

// ============================ main ============================

// 解析 "1920x1200" / "1920x1200@60" 形式的分辨率串
static bool ParseSizeSpec(const std::string &spec, int *w, int *h, int *hz)
{
    const char *p = spec.c_str();
    char *end = nullptr;
    long a = strtol(p, &end, 10);
    if (end == p || (*end != 'x' && *end != 'X')) {
        return false;
    }
    p = end + 1;
    long b = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    long c = 0;
    if (*end == '@') {
        p = end + 1;
        c = strtol(p, &end, 10);
    }
    *w = (int)a;
    *h = (int)b;
    *hz = (int)c;
    return true;
}

struct Args
{
    bool list = false;
    bool vddProbe = false;
    bool vddNoPing = false;
    int output = -1;
    bool vdd = false;
    bool vddFixed = false;     // 强制把虚拟显示器设为 vddW×vddH@vddHz(默认只沿用上次的值)
    int vddW = 946;            // 竖屏默认: 与平板 2800x1840 同比例, 高度与外接屏 1440 对齐
    int vddH = 1440;
    int vddHz = 60;
    std::string side = "left"; // 虚拟显示器摆在外接屏的哪一侧
    bool watch = false;        // 插拔守护模式
    int watchIntervalMs = 1000;
    int unplugDelayMs = 2000;
    bool launchApp = false;    // 检测到设备时尝试拉起平板端 App
    bool presetCmd = false;    // --vdd-preset 子命令
    std::string presetSub;
    std::string presetSpec;
    bool topoCmd = false;      // --topology 子命令: 精确开关某块显示器
    std::string topoSub;
    std::string topoKey;
    bool gui = false;          // --gui: 打开可视化窗口
    bool guiAutoStart = false; // --gui-autostart: 打开窗口并立刻开始(自检用)
    int fps = 30;
    int bitrate = 0;
    std::string source;   // --source: 指定捕获哪块屏(型号串 / 分辨率 WxH / \\.\DISPLAYn)
    int scale = 1;        // --scale: 整数降采样 1/2/4, 降低编码解码负担
    int outW = 0;         // --size: 推流尺寸(GPU 缩放到任意尺寸, 不是整数倍也行)
    int outH = 0;
    std::string dump;     // --dump: 导出一帧(BGRA+NV12)用于离线查颜色, 导完即退出
    std::string host = "127.0.0.1";
    int port = 53517;
    std::string hdc;
    bool nohdc = false;
    std::wstring encoder = L"Microsoft"; // 默认首选 Microsoft AVC DX12(实测每帧泄漏 1.66MB, 仅为 QSV 4.71MB 的 1/3,
                                        // 且跑 RTX 硬编)。--encoder Intel/NVIDIA 可改。
    int duration = 0;
    int sessionLifetimeSec = 0; // 单轮会话最长秒数(0=不限)。GUI 用于定期重建, 规避未知的慢泄漏
    bool noEncode = false;      // --no-encode: 只捕获不编码不推流(泄漏二分诊断: 捕获路径 vs MFT)
    bool noNet = false;         // --no-net: 捕获+编码但不连网(泄漏自测装置, 无需平板; 每帧 ForceDirty 保证满帧率)
    bool sysmemInput = false;   // --sysmem: 强制走旧的"每帧系统内存输入"路径(与 D3D 纹理输入 A/B 对照用)
};

static Args ParseArgs(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        // 分辨率是可选的: 只有下一个参数以数字开头时才吃进来, 避免吞掉后面的开关
        auto nextSizeIfPresent = [&](int *w, int *h, int *hz) {
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                std::string spec = next();
                int tw = 0, th = 0, tz = 0;
                if (ParseSizeSpec(spec, &tw, &th, &tz) && tw >= 320 && tw <= 8192 && th >= 240 &&
                    th <= 8192) {
                    *w = tw;
                    *h = th;
                    if (tz >= 24 && tz <= 240) {
                        *hz = tz;
                    }
                }
            }
        };
        if (s == "--list") {
            a.list = true;
        } else if (s == "--vdd-probe") {
            a.vddProbe = true;
        } else if (s == "--vdd-noping") {
            a.vddProbe = true;
            a.vddNoPing = true;
        } else if (s == "--vdd-preset") {
            a.presetCmd = true;
            a.presetSub = next();
            if (a.presetSub == "set" || a.presetSub == "add" || a.presetSub == "remove") {
                a.presetSpec = next();
            }
        } else if (s == "--topology") {
            a.topoCmd = true;
            a.topoSub = next();
            if (a.topoSub == "on" || a.topoSub == "off") {
                a.topoKey = next();
            } else if (a.topoSub == "gpu") {
                a.topoKey = next(); // "显示器关键字|显卡关键字"
            }
        } else if (s == "--gui") {
            a.gui = true;
        } else if (s == "--gui-autostart") {
            a.gui = true;
            a.guiAutoStart = true;
        } else if (s == "--watch") {
            a.watch = true;
            a.vdd = true; // 守护模式必然用虚拟显示器
        } else if (s == "--watch-interval") {
            a.watchIntervalMs = atoi(next().c_str());
            if (a.watchIntervalMs < 200) {
                a.watchIntervalMs = 200;
            }
            if (a.watchIntervalMs > 10000) {
                a.watchIntervalMs = 10000;
            }
        } else if (s == "--unplug-delay") {
            a.unplugDelayMs = atoi(next().c_str());
            if (a.unplugDelayMs < 200) {
                a.unplugDelayMs = 200;
            }
            if (a.unplugDelayMs > 30000) {
                a.unplugDelayMs = 30000;
            }
        } else if (s == "--launch-app") {
            a.launchApp = true;
        } else if (s == "--side") {
            std::string v = next();
            a.side = (v == "right" || v == "r") ? "right" : "left";
        } else if (s == "--vdd-fixed") {
            a.vdd = true;
            a.vddFixed = true;
            nextSizeIfPresent(&a.vddW, &a.vddH, &a.vddHz);
        } else if (s == "--output") {
            a.output = atoi(next().c_str());
        } else if (s == "--vdd" || s == "--extend") {
            a.vdd = true;
            nextSizeIfPresent(&a.vddW, &a.vddH, &a.vddHz);
        } else if (s == "--source") {
            a.source = next();
        } else if (s == "--scale") {
            a.scale = atoi(next().c_str());
        } else if (s == "--size") {
            // 任意推流尺寸, 由 GPU 缩放(不要求整数倍)。比 --scale 灵活得多:
            // 可以在"清晰度"和"编码耗时"之间取中间档, 例如 1920x1200。
            std::string spec = next();
            int tw = 0, th = 0, tz = 0;
            if (ParseSizeSpec(spec, &tw, &th, &tz) && tw >= 320 && th >= 240) {
                a.outW = tw;
                a.outH = th;
            } else {
                Log("[提示] --size 需要写成 WxH(如 1920x1200), 忽略 \"%s\"", spec.c_str());
            }
        } else if (s == "--dump") {
            a.dump = next();
        } else if (s == "--quality") {
            // 画质预设。后面若再出现 --fps/--bitrate/--scale 会覆盖对应项。
            std::string q = next();
            if (q == "low") {
                a.fps = 30;
                a.scale = 2;
                a.bitrate = 5000;
            } else if (q == "mid") {
                a.fps = 30;
                a.scale = 1;
                a.bitrate = 8000;
            } else if (q == "high") {
                a.fps = 60;
                a.scale = 1;
                a.bitrate = 20000;
            } else {
                Log("[提示] --quality 只支持 low/mid/high, 忽略 \"%s\"", q.c_str());
            }
        } else if (s == "--fps") {
            a.fps = atoi(next().c_str());
            if (a.fps <= 0 || a.fps > 120) {
                a.fps = 30;
            }
        } else if (s == "--bitrate") {
            a.bitrate = atoi(next().c_str());
        } else if (s == "--host") {
            a.host = next();
        } else if (s == "--port") {
            a.port = atoi(next().c_str());
        } else if (s == "--hdc") {
            a.hdc = next();
        } else if (s == "--nohdc") {
            a.nohdc = true;
        } else if (s == "--encoder") {
            std::string e = next();
            int sz = MultiByteToWideChar(CP_UTF8, 0, e.c_str(), -1, nullptr, 0);
            std::wstring w(sz - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, e.c_str(), -1, &w[0], sz);
            a.encoder = w;
        } else if (s == "--duration") {
            a.duration = atoi(next().c_str());
        } else if (s == "--session-lifetime") {
            a.sessionLifetimeSec = atoi(next().c_str());
        } else if (s == "--no-encode") {
            a.noEncode = true;
        } else if (s == "--no-net") {
            a.noNet = true;
        } else if (s == "--sysmem") {
            a.sysmemInput = true;
        }
    }
    return a;
}

static BOOL WINAPI CtrlHandler(DWORD)
{
    g_exit = true;
    return TRUE;
}

// ============================ 推流会话 ============================

static int CalcKbps(const Args &args, int w, int h)
{
    int kbps = args.bitrate > 0 ? args.bitrate : (int)((double)w * h * args.fps * 0.19 / 1000.0);
    if (kbps < 2000) {
        kbps = 2000;
    }
    if (kbps > 40000) {
        kbps = 40000;
    }
    return kbps;
}

// 跑一轮"捕获 → 编码 → 推流"。返回编码出的 AU 帧数; outBytes 返回发送字节数。
// 结束条件: Ctrl+C / externalStop 被置位(守护模式下平板被拔出) / 达到 durationSec。
static uint64_t StreamUntil(const Args &args, DupCapture &cap, int w, int h, int kbps,
                            const std::atomic<bool> *externalStop, int durationSec, uint64_t *outBytes)
{
    auto tStart = Clock::now();
    std::atomic<uint64_t> sentBytes{0};
    std::atomic<uint64_t> pushedFrames{0};
    std::atomic<uint64_t> totalAUs{0};
    std::atomic<bool> streamDead{false};
    std::atomic<bool> localStop{false};
    std::atomic<bool> capDead{false}; // 捕获线程意外死亡(如 bad_alloc) → 会话必须立刻终止并重建
    TcpClient tcp;

    auto stopped = [&]() {
        return g_exit.load() || localStop.load() || (externalStop != nullptr && externalStop->load());
    };

    // 捕获线程
    std::thread capThread([&]() {
      try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const auto interval = std::chrono::microseconds(1000000 / args.fps);
        LONGLONG dur = 10000000LL / args.fps;
        LONGLONG idx = 0;
        auto nextTick = Clock::now();
        while (!stopped()) {
            if (g_forceFrame.exchange(false)) {
                cap.ForceDirty();
            }
            if (args.noNet) {
                cap.ForceDirty(); // 自测模式: 静止桌面也强制每 tick 产帧, 让编码器满载
            }
            nextTick += interval;
            cap.Tick(8);
            if (cap.HasFrame()) {
                FrameBuf f;
                f.nv12.assign(cap.Nv12(), cap.Nv12() + (size_t)w * h * 3 / 2);
                f.ts = idx * dur;
                f.dur = dur;
                f.capT = Clock::now(); // 记录捕获时刻, 用于统计端到端延迟
                idx++;
                pushedFrames.fetch_add(1);
                FeedGlobal(std::move(f));
            }
            std::this_thread::sleep_until(nextTick);
        }
      } catch (const std::bad_alloc &) {
        ProbeMem("capThread");
        capDead = true;
      } catch (const std::exception &e) {
        ProbeLog("exception in capThread: %s", e.what());
        capDead = true;
      } catch (...) {
        ProbeLog("non-std exception in capThread");
        capDead = true;
      }
    });

    // 主循环: 连接 → 编码发送 → 断开重连
    auto lastWaitLog = Clock::now();
    while (!stopped() && !capDead.load()) {
        // 泄漏二分诊断模式: 只跑捕获(+GPU 缩放)路径, 不建编码器不连网。
        // 若此模式 commit 照涨 → 泄漏在捕获/驱动侧; 不涨 → 在编码器(MFT)侧。
        if (args.noEncode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            ProbeLog("stats(noencode): capFrames=%llu commit=%lluMB", (unsigned long long)cap.FrameN(),
                     ProbeCommitMB());
            if (durationSec > 0 &&
                std::chrono::duration<double>(Clock::now() - tStart).count() >= durationSec) {
                break;
            }
            continue;
        }
        if (durationSec > 0 &&
            std::chrono::duration<double>(Clock::now() - tStart).count() >= durationSec) {
            break;
        }
        if (args.noNet) {
            // 泄漏自测模式: 不连 TCP、不发握手, 直接把编码会话跑满 duration。
            // 用途: 无需平板即可复现/验证泄漏(probe 里每 2 秒的 in/out/commit 曲线)。
            streamDead = false;
            H264Encoder enc;
            auto onAu = [&](const uint8_t *, size_t) {
                totalAUs.fetch_add(1); // 只计数, 不发网络
            };
            if (!enc.Create(w, h, args.fps, kbps, args.encoder, onAu, args.sysmemInput)) {
                Log("[自测] 编码器初始化失败, 结束");
                break;
            }
            SetFeedTarget(enc.Queue());
            g_forceFrame = true;
            Log("[自测] 捕获+编码循环开始(无网络), %d 秒后结束", durationSec);
            uint64_t sN = cap.FrameN();
            auto lastStat = Clock::now();
            auto tSess = Clock::now();
            while (!stopped() && !enc.StreamDead() && !capDead.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto now = Clock::now();
                double sec = std::chrono::duration<double>(now - lastStat).count();
                if (sec >= 2.0) {
                    Log("[自测] 编码 %llu 帧 | 捕获 %llu 帧 | 丢帧 %llu | 提交 %lluMB",
                        (unsigned long long)enc.Encoded(), (unsigned long long)cap.FrameN(),
                        (unsigned long long)enc.Dropped(), ProbeCommitMB());
                    ProbeLog("stats(no-net): in=%llu out=%llu commit=%lluMB",
                             (unsigned long long)pushedFrames.load(),
                             (unsigned long long)totalAUs.load(), ProbeCommitMB());
                    lastStat = now;
                }
                if (args.sessionLifetimeSec > 0 &&
                    std::chrono::duration<double>(now - tSess).count() >= args.sessionLifetimeSec) {
                    break; // 主动结束本轮会话, 让外层重建(验证销毁能否回收泄漏)
                }
                if (durationSec > 0 &&
                    std::chrono::duration<double>(Clock::now() - tStart).count() >= durationSec) {
                    break;
                }
                if (cap.ModeChanged()) {
                    break;
                }
            }
            SetFeedTarget(nullptr);
            uint64_t commitBefore = ProbeCommitMB();
            enc.Destroy();
            uint64_t commitAfter = ProbeCommitMB();
            Log("[自测] 会话销毁: 前 %lluMB → 后 %lluMB (回收 %lldMB)", commitBefore, commitAfter,
                (long long)(commitBefore - commitAfter));
            ProbeLog("session end: before=%lluMB after=%lluMB reclaimed=%lldMB", commitBefore,
                     commitAfter, (long long)(commitBefore - commitAfter));
            (void)sN;
            if (args.sessionLifetimeSec > 0 && !stopped() && !capDead.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue; // 重建会话接着跑, 直到总时长到
            }
            break;
        }
        if (!tcp.Connect(args.host.c_str(), args.port)) {
            // 平板端 App 还没监听时(例如锁屏被冻结), 这里会一直重试。
            // 此时虚拟显示器保持存在, 所以"插入即出屏"仍然成立, 只是暂时没画面。
            if (std::chrono::duration<double>(Clock::now() - lastWaitLog).count() >= 5.0) {
                Log("等待平板端连接 %s:%d ...", args.host.c_str(), args.port);
                lastWaitLog = Clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        Log("已连接, 发送握手...");
        char hello[128];
        snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"width\":%d,\"height\":%d}", w, h);
        size_t hl = strlen(hello);
        uint8_t hdr[4] = {(uint8_t)(hl >> 24), (uint8_t)(hl >> 16), (uint8_t)(hl >> 8), (uint8_t)hl};
        if (!tcp.SendAll(hdr, 4) || !tcp.SendAll(hello, hl)) {
            tcp.Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        streamDead = false;
        H264Encoder enc;
        auto onAu = [&](const uint8_t *p, size_t n) {
            uint8_t h2[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
            if (!tcp.SendAll(h2, 4) || !tcp.SendAll(p, n)) {
                streamDead = true;
                return;
            }
            sentBytes.fetch_add(n + 4);
            totalAUs.fetch_add(1);
        };
        if (!enc.Create(w, h, args.fps, kbps, args.encoder, onAu, args.sysmemInput)) {
            tcp.Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        SetFeedTarget(enc.Queue());
        g_forceFrame = true;

        uint64_t lastBytes = 0, lastEnc = 0;
        uint64_t sMap = cap.MapUs(), sCur = cap.CurUs(), sConv = cap.ConvUs(), sN = cap.FrameN();
        auto lastStat = Clock::now();
        auto tStream = Clock::now();
        while (!stopped() && !streamDead.load() && !enc.StreamDead() && !capDead.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (ProbeCommitMB() > 28000) {
                // 编码器栈存在每帧提交泄漏(Intel QSV ≈4.7MB/帧, MS DX12 ≈1.7MB/帧),
                // 且实测(2026-09-15)销毁编码器**不回收**, 只有进程退出能回收。
                // 所以: 会话重建(快, 无感)兜日常, 进程重启(慢, 有感)兜总账。
                // 水位 28GB(系统提交上限 ≈39GB, 留 11GB 给其他程序)。
                RestartProcess("提交内存超过 28GB 水位");
                break;
            }
            auto now = Clock::now();
            double sec = std::chrono::duration<double>(now - lastStat).count();
            if (sec >= 2.0) {
                uint64_t b = sentBytes.load();
                uint64_t e = enc.Encoded();
                // 单帧耗时拆解: 读回+搬运 / 鼠标合成 / BGRA→NV12 转换
                uint64_t m2 = cap.MapUs(), c2 = cap.CurUs(), v2 = cap.ConvUs(), n2 = cap.FrameN();
                uint64_t dn = n2 - sN;
                double pf = dn ? 1.0 / (double)dn : 0.0;
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                Log("统计: 编码 %.1ffps | %.1f Mbps | 丢帧 %llu | 发送端延迟 %.1fms | 单帧[搬运 %.1f 合成 %.1f 转换 %.1f]ms | GPU帧 %llu 失败 %llu | 提交 %lluMB",
                    (double)(e - lastEnc) / sec, (double)(b - lastBytes) * 8 / sec / 1e6,
                    (unsigned long long)enc.Dropped(), enc.AvgLatencyMs(),
                    (double)(m2 - sMap) * pf / 1000.0, (double)(c2 - sCur) * pf / 1000.0,
                    (double)(v2 - sConv) * pf / 1000.0, (unsigned long long)cap.GpuFrames(),
                    (unsigned long long)cap.GpuFails(),
                    (unsigned long long)(pmc.PagefileUsage / 1048576));
                // 泄漏定位遥测: 喂入/取出帧数差 + 提交内存, 写进 probe 供离线对账
                ProbeLog("stats: in=%llu out=%llu commit=%lluMB", (unsigned long long)pushedFrames.load(),
                         (unsigned long long)totalAUs.load(), ProbeCommitMB());
                sMap = m2, sCur = c2, sConv = v2, sN = n2;
                lastBytes = b;
                lastEnc = e;
                lastStat = now;
            }
            if (durationSec > 0 && std::chrono::duration<double>(now - tStream).count() >= durationSec) {
                break;
            }
            if (args.sessionLifetimeSec > 0 &&
                std::chrono::duration<double>(now - tStream).count() >= args.sessionLifetimeSec) {
                Log("已达单会话时长上限(%ds), 主动重建释放资源", args.sessionLifetimeSec);
                ProbeLog("session lifetime reached (%ds), commit=%lluMB", args.sessionLifetimeSec,
                         ProbeCommitMB());
                break;
            }
            if (cap.ModeChanged()) {
                Log("分辨率已被改动, 结束本轮会话以便按新尺寸重建(守护模式会自动重建)");
                break;
            }
        }
        SetFeedTarget(nullptr);
        enc.Destroy();
        tcp.Close();
        if (stopped()) {
            break;
        }
        Log("连接断开, 1 秒后重连...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    localStop = true;
    capThread.join();
    if (outBytes != nullptr) {
        *outBytes = sentBytes.load();
    }
    return totalAUs.load();
}

// ============================ 插拔守护模式 ============================
//
// 需求: 平板(USB)插上 → Windows 里自动多出"第三块屏"并开始投流; 拔下 → 自动销毁。
//
// 为什么这样设计:
//   * 驱动要求宿主 <100ms ping 一次, 否则虚拟显示器约 1 秒后被自动拔掉。所以
//     "进程活着"就等于"屏在", "进程退出/崩溃"就等于"屏自动没了" —— 天然不残留脏状态,
//     拔出方向的清理几乎是白送的。
//   * 设备检测用 hdc list targets(带超时) 而不是 WM_DEVICECHANGE: 前者是"设备真的能被
//     hdc 用"的判据, 后者只说明 USB 枚举了。代价是要轮询, 1 秒一次的开销可以忽略。
//   * 拔出方向加防抖: 线材/接口只是抖一下的话, 重建虚拟显示器会让放在副屏上的窗口
//     跳回主屏, 而且不会自己回去 —— 每次抖动都重建会毁掉用户的窗口布局。
//   * 异常退避: 驱动/链路持续失败时不能疯狂重试(每次失败要等 12~40 秒枚举超时)。

// 一次投屏会话: 建屏 → 捕获 → 推流 → (设备消失/失败) → 清理。
// 返回 0 表示会话正常建立(结束原因通常是拔出或 Ctrl+C), 非 0 表示建立阶段失败。
static int WatchSession(const Args &args, const std::string &hdc)
{
    // VddDisplay 必须先于 DupCapture 声明: 析构逆序保证先放开复制通道、再移除虚拟显示器
    VddDisplay vdd;
    DupCapture cap;

    std::atomic<bool> deviceGone{false};
    std::atomic<bool> monStop{false};
    std::thread mon;

    auto stopMon = [&]() {
        monStop = true;
        if (mon.joinable()) {
            mon.join();
        }
    };

    // 设备监视线程: 连续两次看不到设备、且防抖后仍看不到, 才判定"已拔出"
    mon = std::thread([&]() {
        int miss = 0;
        while (!g_exit.load() && !monStop.load()) {
            SleepInterruptible(args.watchIntervalMs);
            if (g_exit.load() || monStop.load()) {
                return;
            }
            if (HdcDevicePresent(hdc)) {
                miss = 0;
                continue;
            }
            miss++;
            if (miss < 2) {
                continue; // 单次探测失败可能是 hdc 抖动, 再确认一次
            }
            Log("[拔出] hdc 已看不到平板, 等 %d ms 确认...", args.unplugDelayMs);
            SleepInterruptible(args.unplugDelayMs);
            if (g_exit.load() || monStop.load()) {
                return;
            }
            if (HdcDevicePresent(hdc)) {
                miss = 0;
                continue;
            }
            deviceGone = true;
            return;
        }
    });

    VddOptions opt;
    opt.w = args.vddW;
    opt.h = args.vddH;
    opt.hz = args.vddHz;
    opt.fixedMode = args.vddFixed;
    opt.side = args.side;

    int outIndex = -1;
    if (!vdd.Create(opt, &outIndex)) {
        stopMon();
        return 3;
    }
    int w = 0, h = 0;
    if (!cap.Init(outIndex, args.source, args.scale, args.outW, args.outH, &w, &h)) {
        stopMon();
        return 3;
    }
    int kbps = CalcKbps(args, w, h);
    Log("开始推流 %d×%d@%d %dkbps → %s:%d (拔线即自动销毁, Ctrl+C 退出)", w, h, args.fps, kbps,
        args.host.c_str(), args.port);

    uint64_t bytes = 0;
    uint64_t aus = StreamUntil(args, cap, w, h, kbps, &deviceGone, 0, &bytes);
    stopMon();
    cap.Shutdown(); // 先放开捕获通道, vdd 析构才能安全移除虚拟显示器
    Log("会话结束: 编码 %llu 帧, 发送 %.1f MB", (unsigned long long)aus, (double)bytes / 1024 / 1024);
    return 0;
}

static int WatchLoop(const Args &args)
{
    std::string hdc = FindHdc(args.hdc);
    if (hdc.empty()) {
        Log("[错误] 未找到 hdc.exe, 守护模式无法检测平板 (用 --hdc 指定完整路径)");
        return 3;
    }
    Log("===== 插拔守护模式 =====");
    Log("检测方式: \"%s\" list targets, 每 %d ms 一次", hdc.c_str(), args.watchIntervalMs);
    Log("摆位: 外接屏%s | 拔出去抖: %d ms", args.side == "right" ? "右侧" : "左侧", args.unplugDelayMs);
    Log("虚拟显示器: %dx%d@%d (%s)", args.vddW, args.vddH, args.vddHz,
        args.vddFixed ? "强制设置" : "沿用上次设置/Windows 记忆");

    bool online = false;
    int failRun = 0;
    while (!g_exit.load()) {
        if (!HdcDevicePresent(hdc)) {
            if (online) {
                Log("平板已拔出");
                online = false;
            }
            failRun = 0;
            SleepInterruptible(args.watchIntervalMs);
            continue;
        }
        if (!online) {
            Log("检测到平板, 建立链路...");
            online = true;
            if (args.launchApp) {
                std::string o;
                // 锁屏时这一步会失败, 属正常现象, 不影响后续流程
                RunCapture(hdc, "shell aa start -a EntryAbility -b com.example.subscreen", 8000, &o);
            }
        }
        EnsureFport(hdc, args.port);
        if (!FportActive(hdc, args.port)) {
            Log("[警告] fport 未就绪, 稍后重试");
            SleepInterruptible(args.watchIntervalMs);
            continue;
        }

        int rc = WatchSession(args, hdc);
        if (g_exit.load()) {
            break;
        }
        RemoveFport(hdc, args.port);

        bool stillThere = HdcDevicePresent(hdc);
        if (!stillThere && online) {
            Log("平板已拔出");
            online = false;
        }
        if (rc != 0 && stillThere) {
            failRun++;
            int backoff = 1500 * failRun;
            if (backoff > 20000) {
                backoff = 20000;
            }
            Log("会话建立失败(code=%d), %d ms 后重试", rc, backoff);
            SleepInterruptible(backoff);
        } else {
            failRun = 0;
            SleepInterruptible(1000);
        }
    }
    Log("守护模式退出");
    return 0;
}

// 跑一轮"捕获→编码→推流"。命令行与 GUI 共用。
// externalStop 非空时, 置位即可让本函数尽快返回(GUI 的"停止副屏")。
// 返回 0 正常, 3 初始化失败, 4 一帧都没编出来。
static int RunMirrorSession(const Args &args, const std::atomic<bool> *externalStop, bool removeFportOnExit)
{
    DupCapture cap;
    int w = 0, h = 0;
    if (!cap.Init(args.output, args.source, args.scale, args.outW, args.outH, &w, &h)) {
        return 3;
    }
    int kbps = CalcKbps(args, w, h);

    std::string hdc = FindHdc(args.hdc);
    if (!args.nohdc && !args.noNet) {
        if (hdc.empty()) {
            Log("[警告] 未找到 hdc.exe, 请手动执行 fport 或用 --hdc 指定路径");
        } else {
            EnsureFport(hdc, args.port);
            Log("fport 转发已建立 (tcp:%d → 设备)", args.port);
        }
    }

    Log("开始推流 %d×%d@%d %dkbps → %s:%d", w, h, args.fps, kbps, args.host.c_str(), args.port);
    uint64_t bytes = 0;
    uint64_t totalAUs = StreamUntil(args, cap, w, h, kbps, externalStop, args.duration, &bytes);
    bool modeChanged = cap.ModeChanged();
    cap.Shutdown();
    Log("结束: 编码 %llu 帧, 发送 %.1f MB", (unsigned long long)totalAUs, (double)bytes / 1024 / 1024);
    if (modeChanged) {
        Log("[提示] 显示器分辨率在运行中被改动, 重新运行即可按新尺寸推流");
    }
    if (removeFportOnExit && !args.nohdc && !hdc.empty()) {
        RemoveFport(hdc, args.port);
    }
    return (totalAUs > 0) ? 0 : 4;
}

// ============================ 可视化窗口(GUI) ============================
//
// 一键启动的逻辑(用户要求):
//   点"启动副屏" → ① 检查平板(USB 在线 + App 在跑)
//                 ② 把笔记本内屏接入桌面(双屏扩展) —— 精确只动这一块, 不唤醒其他虚拟屏
//                 ③ 开始推流(把内屏镜像到平板)
//   点"停止副屏"或关窗口 → 停流 + 恢复"仅显示屏"
//
// 纯 Win32 原生控件, 不引入 UI 框架; 全程不需要管理员(与镜像方案一致)。

static const char *kGuiAppBundle = "com.example.subscreen";
static const int kGuiPort = 53517;

struct GuiPreset
{
    const wchar_t *label;
    const char *size; // "WxH"; 空表示不缩放
    int fps;
};
static const GuiPreset kGuiPresets[] = {
    {L"1920x1200 @45fps   推荐: 跟手且清晰", "1920x1200", 45},
    {L"2048x1280 @45fps   再清晰一点, 偶尔丢帧", "2048x1280", 45},
    {L"2560x1600 @30fps   1:1 像素完美, 较慢", "2560x1600", 30},
    {L"1280x800  @60fps   最跟手, 画面偏糊", "1280x800", 60},
};

struct GuiRuntime
{
    HWND hwnd = nullptr;
    HWND devBox = nullptr;
    HWND stageBox = nullptr;
    HWND btn = nullptr;
    HWND sizeLabel = nullptr;
    HWND sizeBox = nullptr;
    HWND chkBox = nullptr;
    HWND logBox = nullptr;
    HFONT font = nullptr;

    std::wstring source = L"NE160QDM"; // 内屏关键字(EDID 型号)
    std::string sizeSpec = "1920x1200";
    int fps = 45;
    std::atomic<bool> wantPortrait{true}; // 启动时把内屏转成纵向 90°

    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> busy{false};
    std::atomic<bool> didExtend{false};
    std::atomic<bool> running{false}; // 推流会话进行中(托盘图标绿/红的依据)

    std::mutex mx;
    std::wstring stage = L"就绪";
    std::wstring device = L"尚未检查";
};
static GuiRuntime g_gui;
static bool g_guiAutoStart = false; // 窗口显示后立即开始一次(自检用, 等价于点按钮)

static void GuiStage(const wchar_t *s)
{
    std::lock_guard<std::mutex> lk(g_gui.mx);
    g_gui.stage = s;
}

static void GuiDevice(const wchar_t *s)
{
    std::lock_guard<std::mutex> lk(g_gui.mx);
    g_gui.device = s;
}

// 内屏是否已经出现在活动拓扑里(按关键字匹配)
static bool PanelIsActive(const std::wstring &key)
{
    for (auto &t : CcdEnumTargets()) {
        if (!t.active) {
            continue;
        }
        if (WStrContainsNoCase(t.pathId + L"|" + t.name, key)) {
            return true;
        }
    }
    return false;
}

// ============================ 托盘图标 ============================
//
// 用户要求的形态: 双击启动 → 控制窗口; 点「启动副屏」→ 窗口收起, 常驻托盘;
// 托盘右键 = 打开控制窗口 / 开始·结束推流 / 退出。托盘图标本身是状态灯
// (绿=推流中, 红=未推流), 与平板端的角落圆点呼应。

static const UINT WM_APP_TRAY = WM_APP + 1;
static NOTIFYICONDATAW g_nid{};
static bool g_trayOn = false;
static HICON g_trayIconGreen = nullptr;
static HICON g_trayIconRed = nullptr;
static const int IDM_TRAY_OPEN = 2001;
static const int IDM_TRAY_TOGGLE = 2002;
static const int IDM_TRAY_EXIT = 2003;

// 程序化画一枚实心圆点图标(不用资源文件)
static HICON MakeDotIcon(COLORREF fill)
{
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 8) {
        sz = 16;
    }
    HICON icon = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC cdc = CreateCompatibleDC(sdc);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP cbm = CreateCompatibleBitmap(sdc, sz, sz);
    HBITMAP mbm = CreateBitmap(sz, sz, 1, 1, nullptr);
    HBITMAP oc = (HBITMAP)SelectObject(cdc, cbm);
    HBITMAP om = (HBITMAP)SelectObject(mdc, mbm);
    RECT rc{0, 0, sz, sz};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(cdc, &rc, black);
    FillRect(mdc, &rc, white); // AND 掩码默认全透明(1), 圆内用 0(不透明)
    SelectObject(cdc, br);
    SelectObject(mdc, black);
    Ellipse(cdc, 1, 1, sz - 1, sz - 1);
    Ellipse(mdc, 1, 1, sz - 1, sz - 1);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = cbm;
    ii.hbmMask = mbm;
    icon = CreateIconIndirect(&ii);
    DeleteObject(black);
    DeleteObject(white);
    DeleteObject(br);
    SelectObject(cdc, oc);
    SelectObject(mdc, om);
    DeleteObject(cbm);
    DeleteObject(mbm);
    DeleteDC(cdc);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return icon;
}

static void TrayAdd(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_trayIconRed ? g_trayIconRed : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcsncpy(g_nid.szTip, L"SubScreen 副屏控制台", 127);
    g_trayOn = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void TrayRemove()
{
    if (g_trayOn) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayOn = false;
    }
}

// 状态同步: 绿=推流中, 红=未推流(只在状态翻转时改, 不每 300ms 骚扰 shell)
static void TraySet(bool running)
{
    if (!g_trayOn) {
        return;
    }
    static bool s_last = false;
    static bool s_init = false;
    if (s_init && s_last == running) {
        return;
    }
    s_last = running;
    s_init = true;
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = running ? g_trayIconGreen : g_trayIconRed;
    wcsncpy(g_nid.szTip, running ? L"SubScreen 副屏 - 推流中" : L"SubScreen 副屏 - 未推流", 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayBalloon(const wchar_t *title, const wchar_t *msg)
{
    if (!g_trayOn) {
        return;
    }
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, 63);
    wcsncpy(g_nid.szInfo, msg, 255);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP; // 恢复常规标志, 气泡超时后自动消失
}

static void GuiShowWindow()
{
    if (!g_gui.hwnd) {
        return;
    }
    ShowWindow(g_gui.hwnd, SW_SHOW);
    SetForegroundWindow(g_gui.hwnd);
}

// 工作线程: 检查 → 接内屏 → 推流 → 恢复
static void GuiWorker()
{
    Log("================ 启动副屏 ================");

    // ① 平板是否插着
    GuiStage(L"① 检查平板连接…");
    std::string hdc = FindHdc(std::string());
    if (hdc.empty()) {
        GuiDevice(L"平板: 未找到 hdc.exe");
        GuiStage(L"✗ 找不到 hdc.exe —— 请确认已安装 DevEco Studio");
        g_gui.busy = false;
        return;
    }
    if (!HdcDevicePresent(hdc)) {
        GuiDevice(L"平板: 未检测到");
        GuiStage(L"✗ 没有检测到平板 —— 请插好 USB 线, 并解锁屏幕");
        g_gui.busy = false;
        return;
    }
    GuiDevice(L"平板: USB 已连接");

    // ② 平板端 App 是否真的在监听(比"设备在线"更硬的判据)
    GuiStage(L"② 检查平板端 App…");
    EnsureFport(hdc, kGuiPort);
    bool appAlive = false;
    for (int i = 0; i < 6 && !g_gui.stop.load(); i++) {
        if (TcpPortAlive("127.0.0.1", kGuiPort, 1500)) {
            appAlive = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    if (!appAlive) {
        appAlive = HdcAppRunning(hdc, kGuiAppBundle);
    }
    if (!appAlive) {
        GuiDevice(L"平板: 已连接, 但 App 未运行");
        GuiStage(L"✗ 平板端 App 没在运行 —— 请在平板上打开 SubScreen 并让它在前台");
        g_gui.busy = false;
        return;
    }
    GuiDevice(L"平板: 已连接, App 正在运行");

    // ③ 把笔记本内屏接入桌面(= 双屏扩展)
    GuiStage(L"③ 把笔记本内屏接入桌面(双屏扩展)…");
    std::string err;
    if (!PanelIsActive(g_gui.source)) {
        if (CcdSetTargetActive(g_gui.source, true, &err)) {
            Log("[拓扑] 已精确接入内屏(只动这一块屏)");
        } else {
            Log("[拓扑] 精确接入失败: %s", err.c_str());
            Log("[拓扑] 退化为整体扩展 SDC_TOPOLOGY_EXTEND");
            LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
            if (rc != ERROR_SUCCESS) {
                GuiStage(L"✗ 无法切换到双屏扩展");
                g_gui.busy = false;
                return;
            }
        }
        // 等它真的挂上桌面(拓扑提交是异步生效的)
        for (int i = 0; i < 24 && !g_gui.stop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (PanelIsActive(g_gui.source)) {
                break;
            }
        }
    } else {
        Log("[拓扑] 内屏已经是活动输出, 无需切换");
    }
    if (!PanelIsActive(g_gui.source)) {
        GuiStage(L"✗ 内屏没能接入桌面");
        g_gui.busy = false;
        return;
    }
    // 用户要求的语义: 只要进入过推流, 停止时一律恢复"仅显示屏" ——
    // 哪怕这块屏在我们启动前就是亮的(否则点停止后内屏会一直残留)。
    g_gui.didExtend = true;

    // ③b 内屏方向: 竖屏(纵向 90°)。纯映射模式下平板看到的画面方向完全由这里决定。
    // (接入时系统挑默认模式会把方向重置成横向, 所以这里显式转回来。)
    if (g_gui.wantPortrait.load()) {
        GuiStage(L"③b 把内屏转为竖屏(90°)…");
        std::string oerr;
        if (SetPanelOrientation(g_gui.source, DMDO_90, &oerr)) {
            Log("[方向] 内屏已设为纵向(90°)");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // 等模式切换稳定, 捕获按新方向初始化
        } else {
            Log("[方向] 设置竖屏失败: %s (按当前方向继续推流)", oerr.c_str());
        }
    }

    // ④ 推流
    GuiStage(L"④ 正在推流…(点「停止副屏」结束)");
    // 窗口使命完成: 收进托盘(用户要求: 点开始推流后前台窗口消失, 常驻托盘小标)
    g_gui.running = true;
    if (g_gui.hwnd && IsWindowVisible(g_gui.hwnd)) {
        ShowWindow(g_gui.hwnd, SW_HIDE);
        TrayBalloon(L"SubScreen 副屏", L"已开始推流, 已收到托盘。\n双击托盘图标可打开控制窗口。");
    }
    Args a;
    {
        // --source 关键字是 UTF-8 窄串, 这里把宽串转回去
        int n = WideCharToMultiByte(CP_UTF8, 0, g_gui.source.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) {
            WideCharToMultiByte(CP_UTF8, 0, g_gui.source.c_str(), -1, &s[0], n, nullptr, nullptr);
            s.resize((size_t)n - 1);
        }
        a.source = s;
    }
    a.fps = g_gui.fps > 0 ? g_gui.fps : 30;
    a.port = kGuiPort;
    int tw = 0, th = 0, tz = 0;
    if (ParseSizeSpec(g_gui.sizeSpec, &tw, &th, &tz) && tw >= 320 && th >= 240) {
        a.outW = tw;
        a.outH = th;
    }
    // 推流循环 + 自愈: 会话因异常(如捕获线程 bad_alloc / 编码器死)结束时,
    // 自动重建捕获与编码器继续推, 而不是停在"有连接没画面"的假活状态。
    // 另配单会话时长上限(150s < bad_alloc 的 190s): 定期主动重建, 重建前后的提交内存
    // 写进 probe —— 如果重建后内存回落, 说明泄漏随编码器销毁释放(可定期续命);
    // 如果不回落, 泄漏在进程级(驱动/堆), 需要另查。
    a.sessionLifetimeSec = 150;
    int rc = 0;
    int attempt = 0;
    while (!g_gui.stop.load() && attempt < 50) {
        if (ProbeCommitMB() > 28000) {
            // 会话重建释放不了泄漏(MFT 之外), 只能进程重启(系统回收全部提交)
            RestartProcess("重建前提交内存超过 28GB 水位");
            break;
        }
        rc = RunMirrorSession(a, &g_gui.stop, false);
        if (g_gui.stop.load()) {
            break;
        }
        attempt++;
        uint64_t c1 = ProbeCommitMB();
        Log("会话结束(rc=%d, 提交 %lluMB), 3 秒后自动重建(第 %d 次)", rc, c1, attempt);
        for (int i = 0; i < 3 && !g_gui.stop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ProbeLog("session restart #%d (rc=%d) commitBefore=%lluMB commitAfter=%lluMB", attempt, rc, c1,
                 ProbeCommitMB());
    }
    Log("推流结束 (rc=%d, 重建 %d 次)", rc, attempt);
    g_gui.running = false;
    bool wasStop = g_gui.stop.load();

    // ⑤ 恢复"仅显示屏"
    if (g_gui.didExtend.load()) {
        GuiStage(L"⑤ 正在恢复「仅显示屏」…");
        std::string e2;
        if (CcdSetTargetActive(g_gui.source, false, &e2)) {
            Log("[拓扑] 已恢复: 内屏移出桌面");
        } else {
            Log("[拓扑] 恢复失败: %s (可在 Windows 显示设置里手动改回)", e2.c_str());
        }
        g_gui.didExtend = false;
    }
    g_gui.busy = false;
    GuiStage(L"已停止");
    GuiDevice(L"平板: 已连接, App 正在运行");
    // 托盘行为: 窗口在托盘里时, 正常停止只冒泡不弹窗; 异常结束则弹回控制窗口给用户看现场
    if (g_gui.hwnd && !IsWindowVisible(g_gui.hwnd)) {
        if (wasStop) {
            TrayBalloon(L"SubScreen 副屏", L"已停止推流, 内屏已恢复。\n双击托盘图标可打开控制窗口。");
        } else {
            GuiShowWindow();
        }
    }
}

// 线程入口包一层异常捕获: 未捕获异常会走 std::terminate → fastfail(0xc0000409),
// 这里拦下来把异常信息写进 crash_probe.log, 就知道是哪种异常、在哪条线程。
static void GuiWorkerTrampoline()
{
    try {
        GuiWorker();
    } catch (const std::bad_alloc &) {
        ProbeMem("GuiWorker");
    } catch (const std::exception &e) {
        ProbeLog("exception in GuiWorker: %s", e.what());
    } catch (...) {
        ProbeLog("non-std exception in GuiWorker");
    }
}

// 停流 + 恢复拓扑(阻塞到工作线程结束; 推流循环 ~500ms 响应一次)
static void GuiStopAndRestore()
{
    if (!g_gui.worker.joinable()) {
        return;
    }
    GuiStage(L"正在停止…");
    g_gui.stop = true;
    g_gui.worker.join();
    g_gui.stop = false;
    g_gui.busy = false;
    // 工作线程里已经恢复过; 万一它异常退出, 这里再兜一次
    if (g_gui.didExtend.exchange(false)) {
        std::string e2;
        CcdSetTargetActive(g_gui.source, false, &e2);
    }
}

// 托盘菜单「退出」: 停流+恢复拓扑 → 移除托盘 → 销毁窗口退出
static void GuiQuit()
{
    GuiStopAndRestore(); // 未推流时它直接返回
    TrayRemove();
    if (g_gui.hwnd) {
        DestroyWindow(g_gui.hwnd); // → WM_DESTROY → PostQuitMessage
    }
}

static void GuiStart()
{
    if (g_gui.busy.load()) {
        return;
    }
    int sel = (int)SendMessageW(g_gui.sizeBox, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)(sizeof(kGuiPresets) / sizeof(kGuiPresets[0]))) {
        g_gui.sizeSpec = kGuiPresets[sel].size;
        g_gui.fps = kGuiPresets[sel].fps;
    }
    g_gui.wantPortrait = SendMessageW(g_gui.chkBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_gui.stop = false;
    g_gui.busy = true;
    GuiStage(L"正在启动…");
    if (g_gui.worker.joinable()) {
        g_gui.worker.join();
    }
    g_gui.worker = std::thread(GuiWorkerTrampoline);
}

static int g_hbCount = 0;

static void GuiRefresh()
{
    std::wstring st, dev;
    {
        std::lock_guard<std::mutex> lk(g_gui.mx);
        st = g_gui.stage;
        dev = g_gui.device;
    }
    TraySet(g_gui.running.load()); // 托盘图标绿/红状态灯
    if (++g_hbCount % 100 == 0) { // 300ms × 100 = 每 30 秒一次心跳, 崩溃后对出最后状态
        char s[256]{};
        WideCharToMultiByte(CP_UTF8, 0, st.c_str(), -1, s, sizeof(s) - 1, nullptr, nullptr);
        ProbeLog("heartbeat: %s", s);
    }
    std::wstring line = L"状态: " + st + L"        " + dev;
    SetWindowTextW(g_gui.stageBox, line.c_str());
    SetWindowTextW(g_gui.btn, g_gui.busy.load() ? L"■  停止副屏" : L"▶  启动副屏");
    EnableWindow(g_gui.sizeBox, g_gui.busy.load() ? FALSE : TRUE);

    std::string chunk;
    GuiLogTake(&chunk);
    if (!chunk.empty()) {
        std::wstring w = Utf8ToWide(chunk);
        int len = GetWindowTextLengthW(g_gui.logBox);
        SendMessageW(g_gui.logBox, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_gui.logBox, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
        SendMessageW(g_gui.logBox, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK GuiProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        int dpi = 96;
        HDC dc = GetDC(hwnd);
        if (dc) {
            dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(hwnd, dc);
        }
        int h = -MulDiv(10, dpi, 72);
        g_gui.font = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Microsoft YaHei UI");

        auto mk = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int hh,
                      int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hh, hwnd,
                                     (HMENU)(INT_PTR)id, nullptr, nullptr);
            if (c && g_gui.font) {
                SendMessageW(c, WM_SETFONT, (WPARAM)g_gui.font, TRUE);
            }
            return c;
        };

        g_gui.sizeLabel = mk(L"STATIC", L"输出规格", SS_LEFT, 16, 16, 70, 22, 0);
        g_gui.sizeBox = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 92, 12, 300, 200, 1004);
        for (auto &p : kGuiPresets) {
            SendMessageW(g_gui.sizeBox, CB_ADDSTRING, 0, (LPARAM)p.label);
        }
        SendMessageW(g_gui.sizeBox, CB_SETCURSEL, 0, 0);

        g_gui.btn = mk(L"BUTTON", L"▶  启动副屏", BS_PUSHBUTTON, 412, 11, 156, 34, 1001);

        g_gui.chkBox = mk(L"BUTTON", L"内屏竖屏(90°)", BS_AUTOCHECKBOX, 16, 57, 150, 22, 1005);
        SendMessageW(g_gui.chkBox, BM_SETCHECK, BST_CHECKED, 0);

        g_gui.stageBox = mk(L"STATIC", L"状态: 就绪", SS_LEFT | SS_ENDELLIPSIS, 176, 59, 392, 22, 0);
        g_gui.logBox = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL |
                                           ES_LEFT | WS_BORDER,
                          16, 88, 552, 280, 1002);
        SendMessageW(g_gui.logBox, EM_SETLIMITTEXT, 200000, 0);

        SetTimer(hwnd, 1, 300, nullptr);

        // 托盘图标随窗口一起常驻(关窗只是收起, 程序还在托盘里)
        g_trayIconGreen = MakeDotIcon(RGB(61, 220, 132));
        g_trayIconRed = MakeDotIcon(RGB(248, 113, 113));
        TrayAdd(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            GuiRefresh();
        }
        return 0;
    case WM_APP_TRAY:
        // 托盘交互: 双击 = 打开控制窗口; 右键 = 菜单
        if (lp == WM_LBUTTONDBLCLK) {
            GuiShowWindow();
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            if (m) {
                AppendMenuW(m, MF_STRING, IDM_TRAY_OPEN, L"打开控制窗口");
                AppendMenuW(m, MF_STRING, IDM_TRAY_TOGGLE,
                            g_gui.busy.load() ? L"结束推流" : L"开始推流");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出");
                SetForegroundWindow(hwnd); // TrackPopupMenu 前必须置前台, 否则点外面不消失
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x,
                                         pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(m);
                if (cmd == IDM_TRAY_OPEN) {
                    GuiShowWindow();
                } else if (cmd == IDM_TRAY_TOGGLE) {
                    // 与窗口上的按钮走完全同一条路径(1001 = 启动/停止二合一)
                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, BN_CLICKED), 0);
                } else if (cmd == IDM_TRAY_EXIT) {
                    GuiQuit();
                }
            }
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 1001 && HIWORD(wp) == BN_CLICKED) {
            if (g_gui.busy.load()) {
                GuiStopAndRestore();
            } else {
                GuiStart();
            }
        }
        return 0;
    case WM_CLOSE:
        // 用户要求: 点开始推流后窗口已收进托盘, 点 X 也一样 —— 只是收起, 不退出不停止。
        // 真正退出只走托盘右键菜单的「退出」(会停流并恢复"仅显示屏")。
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        TrayRemove();
        if (g_trayIconGreen) {
            DestroyIcon(g_trayIconGreen);
            g_trayIconGreen = nullptr;
        }
        if (g_trayIconRed) {
            DestroyIcon(g_trayIconRed);
            g_trayIconRed = nullptr;
        }
        if (g_gui.font) {
            DeleteObject(g_gui.font);
            g_gui.font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunGui()
{
    SetProcessDPIAware(); // 高 DPI 屏上窗口与文字才不糊(必须在创建窗口前调用)

    // 单实例: 两个窗口同时抢拓扑/端口会互相打架
    // (自重启流程会先释放互斥再退出, 这里若撞上就等新互斥生效, 最多 15 秒)
    for (int i = 0; i < 30; i++) {
        g_guiMutex = CreateMutexW(nullptr, TRUE, L"SubScreenGuiSingleton_v1");
        if (!g_guiMutex || GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
        ReleaseMutex(g_guiMutex);
        CloseHandle(g_guiMutex);
        g_guiMutex = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!g_guiMutex) {
        MessageBoxW(nullptr, L"副屏控制台已在运行(等待超时)。", L"SubScreen", MB_ICONINFORMATION);
        return 0;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GuiProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SubScreenGuiWnd";
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败", L"SubScreen", MB_ICONERROR);
        return 3;
    }

    int dpi = 96;
    if (HDC dc = GetDC(nullptr)) {
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
    }
    int cw = MulDiv(584, dpi, 96);
    int chh = MulDiv(404, dpi, 96);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SubScreen 副屏控制台", WS_OVERLAPPED | WS_CAPTION |
                                                                                 WS_SYSMENU | WS_MINIMIZEBOX,
                                (sw - cw) / 2, (sh - chh) / 2, cw, chh, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"窗口创建失败", L"SubScreen", MB_ICONERROR);
        return 3;
    }
    g_gui.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 日志开始收集(必须在窗口建好之后, 免得前面创建期的日志丢失)
    g_logCapture = true;
    Log("SubScreen 副屏控制台已就绪");
    Log("用法: 插好平板 → 在平板上打开 SubScreen 并解锁 → 点\"启动副屏\"");
    Log("说明: 启动时会把笔记本内屏接入桌面(双屏扩展), 停止时自动恢复\"仅显示屏\"");

    if (g_guiAutoStart) {
        GuiStart(); // 与点"启动副屏"完全同一条路径
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_logCapture = false;
    return 0;
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8"); // printf %ls 宽字符(编码器名含®等)按 UTF-8 转换
    std::set_terminate(TerminateProbe);
    ProbeLog("=== process start (pid %lu) ===", GetCurrentProcessId());
    Args args = ParseArgs(argc, argv);

    // /SUBSYSTEM:WINDOWS 下默认没有控制台:
    //   - 被重定向(> log / 管道) → stdout 句柄有效, printf 照常(自动化测试路径);
    //   - 从 cmd 直接跑(未重定向) → 附着父控制台, 输出照样可见;
    //   - 双击 exe(GUI) → 无控制台, 日志只走 GUI 日志框 + sender.log。
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        g_stdoutOK = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE);
        if (!g_stdoutOK && argc > 1 && !args.gui) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                freopen("CONOUT$", "w", stdout);
                g_stdoutOK = true;
            }
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) {
        Log("[错误] COM/MF 初始化失败");
        return 3;
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // 无参数(双击 exe) = 打开可视化窗口。带任何参数时保持原来的命令行行为。
    if (args.gui || argc <= 1) {
        g_guiAutoStart = args.guiAutoStart;
        int rc = RunGui();
        MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (args.list) {
        Log("== 桌面输出 ==");
        for (auto &e : EnumOutputs()) {
            const wchar_t *rot = L"UNSPECIFIED";
            if (e.desc.Rotation == DXGI_MODE_ROTATION_IDENTITY) {
                rot = L"IDENTITY";
            } else if (e.desc.Rotation == DXGI_MODE_ROTATION_ROTATE90) {
                rot = L"ROTATE90";
            } else if (e.desc.Rotation == DXGI_MODE_ROTATION_ROTATE180) {
                rot = L"ROTATE180";
            } else if (e.desc.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
                rot = L"ROTATE270";
            }
            Log("  #%d %ls (%ls) %d×%d @(%ld,%ld) rotation=%ls %ls", e.index, e.deviceName.c_str(),
                e.adapterName.c_str(), e.desc.DesktopCoordinates.right - e.desc.DesktopCoordinates.left,
                e.desc.DesktopCoordinates.bottom - e.desc.DesktopCoordinates.top,
                e.desc.DesktopCoordinates.left, e.desc.DesktopCoordinates.top, rot,
                e.desc.AttachedToDesktop ? L"[已连接]" : L"");
        }
        // 每块屏的稳定标识, 供 --source 直接抄
        {
            auto nameMap = CcdActiveDisplayNames();
            Log("== 捕获源标识 (给 --source 用) ==");
            for (auto &e : EnumOutputs()) {
                std::wstring model;
                for (auto &kv : nameMap) {
                    if (kv.first == e.deviceName) {
                        model = kv.second;
                        break;
                    }
                }
                Log("  #%d  \"%ls\"  或  \"%dx%d\"  %ls", e.index, e.deviceName.c_str(),
                    e.desc.DesktopCoordinates.right - e.desc.DesktopCoordinates.left,
                    e.desc.DesktopCoordinates.bottom - e.desc.DesktopCoordinates.top,
                    model.empty() ? L"" : model.c_str());
            }
        }
        // 列编码器
        Log("== H.264 编码器 ==");
        MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate **arr = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                               MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               &inInfo, &outInfo, &arr, &count);
        if (SUCCEEDED(hr) && count > 0) {
            for (UINT32 i = 0; i < count; i++) {
                wchar_t name[256] = L"(unknown)";
                UINT32 nlen = 0;
                if (SUCCEEDED(arr[i]->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &nlen)) && nlen > 0 &&
                    nlen < 256) {
                    arr[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
                }
                Log("  [硬件] %ls", name);
                arr[i]->Release();
            }
            CoTaskMemFree(arr);
        } else {
            Log("  (无硬件编码器)");
        }
        ListVdd();
        return 0;
    }

    // 注册表预设管理(只读写 HKLM, 不碰显示器)
    if (args.topoCmd) {
        return TopologyCommand(args.topoSub, args.topoKey);
    }

    if (args.presetCmd) {
        return VddPresetCommand(args.presetSub, args.presetSpec);
    }

    if (args.vddProbe) {
        return VddProbe(!args.vddNoPing);
    }

    // 插拔守护模式: 常驻, 插入 USB 自动出屏并推流, 拔出自动销毁
    if (args.watch) {
        int rc = WatchLoop(args);
        MFShutdown();
        CoUninitialize();
        return rc;
    }

    // 单次会话。VddDisplay 必须声明在 DupCapture 之前: 析构逆序保证先放开捕获通道、
    // 再移除虚拟显示器(否则 DXGI 复制通道会指向一个已消失的输出)。
    VddDisplay vdd;
    DupCapture cap;
    int w = 0, h = 0;
    int outIndex = args.output;
    if (args.vdd) {
        if (args.output >= 0) {
            Log("[提示] --vdd 与 --output 同时指定, 已忽略 --output");
        }
        VddOptions opt;
        opt.w = args.vddW;
        opt.h = args.vddH;
        opt.hz = args.vddHz;
        opt.fixedMode = args.vddFixed;
        opt.side = args.side;
        if (!vdd.Create(opt, &outIndex)) {
            return 3;
        }
    }
    if (!cap.Init(outIndex, args.vdd ? std::string() : args.source, args.scale, args.outW, args.outH, &w,
                  &h)) {
        return 3;
    }
    int kbps = CalcKbps(args, w, h);

    // ---- 诊断: --dump <前缀> ----
    // 把"送进编码器的那一帧 NV12"和"它之前的 BGRA"原样导出, 然后退出(不建 fport、不推流)。
    // 目的: 把整条管线切成"发送端内部 / 发送端之后"两段 —— 用 Python 把两者都还原成图片对照,
    // 就能判断颜色问题是本地就存在, 还是编码器/平板解码器引入的。
    if (!args.dump.empty()) {
        for (int i = 0; i < 300 && !cap.HasFrame() && !g_exit.load(); i++) {
            cap.ForceDirty();
            cap.Tick(50);
        }
        if (!cap.HasFrame()) {
            Log("[错误] --dump: 等不到可用的帧");
            return 3;
        }
        std::string base = args.dump;
        {
            FILE *f = fopen((base + ".nv12").c_str(), "wb");
            if (f) {
                fwrite(cap.Nv12(), 1, (size_t)w * h * 3 / 2, f);
                fclose(f);
            } else {
                Log("[错误] 无法写入 %s.nv12", base.c_str());
            }
        }
        if (cap.HasBgra()) {
            FILE *f = fopen((base + ".bgra").c_str(), "wb");
            if (f) {
                fwrite(cap.Bgra(), 1, (size_t)cap.FullWidth() * cap.FullHeight() * 4, f);
                fclose(f);
            }
        } else {
            Log("(本帧走 GPU 路径, 没有回读 BGRA, 只导出 NV12)");
        }
        {
            FILE *f = fopen((base + ".txt").c_str(), "w");
            if (f) {
                fprintf(f, "%d %d %d %d\n", w, h, cap.FullWidth(), cap.FullHeight());
                fclose(f);
            }
        }
        Log("已导出一帧: %s.nv12 (推流 %dx%d) + %s.bgra (桌面 %dx%d)", base.c_str(), w, h, base.c_str(),
            cap.FullWidth(), cap.FullHeight());
        return 0;
    }

    std::string hdc = FindHdc(args.hdc);
    if (!args.nohdc) {
        if (hdc.empty()) {
            Log("[警告] 未找到 hdc.exe, 请手动执行 fport 或用 --hdc 指定路径");
        } else {
            EnsureFport(hdc, args.port);
            Log("fport 转发已建立 (tcp:%d → 设备)", args.port);
        }
    }

    Log("开始推流 %d×%d@%d %dkbps → %s:%d (Ctrl+C 退出)", w, h, args.fps, kbps, args.host.c_str(),
        args.port);

    uint64_t bytes = 0;
    uint64_t totalAUs = StreamUntil(args, cap, w, h, kbps, nullptr, args.duration, &bytes);
    bool modeChanged = cap.ModeChanged(); // Shutdown 会清掉这个标志, 先取出来
    cap.Shutdown();                       // 先放开捕获通道, vdd 析构才能安全移除虚拟显示器

    Log("结束: 编码 %llu 帧, 发送 %.1f MB", (unsigned long long)totalAUs, (double)bytes / 1024 / 1024);
    if (modeChanged) {
        Log("[提示] 显示器分辨率在运行中被改动。重新运行本程序(或改用 --watch)即可按新尺寸推流");
    }

    if (!args.nohdc && !hdc.empty()) {
        RemoveFport(hdc, args.port);
    }
    MFShutdown();
    CoUninitialize();
    return (totalAUs > 0) ? 0 : 4;
}
