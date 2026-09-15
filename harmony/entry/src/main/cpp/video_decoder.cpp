#include "video_decoder.h"

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>  // AV_PIXEL_FORMAT_SURFACE_FORMAT / OH_MD_KEY_PIXEL_FORMAT

#include <cstring>

namespace {
// 只有 1 帧深。深度越大越"平滑", 但每一帧排队都是**用户直接感知到的延迟** ——
// 投屏要的是跟手, 所以解码器忙不过来时宁可丢掉上一帧, 永远只解最新的一帧。
constexpr size_t MAX_QUEUED_FRAMES = 1;
constexpr uint64_t FRAME_WARN_INTERVAL_US = 2000000; // 告警打印间隔
}

#define LOG(fmt, ...) ((void)OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "SubScreen", fmt, ##__VA_ARGS__))
#define LOGE(fmt, ...) ((void)OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "SubScreen", fmt, ##__VA_ARGS__))

bool VideoDecoder::Start(int width, int height, OHNativeWindow *window)
{
    if (running_.exchange(true)) {
        return false;
    }
    framesDecoded.store(0);
    lastError.store(0);

    codec_ = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (codec_ == nullptr) {
        LOGE("create decoder failed");
        running_ = false;
        return false;
    }

    int32_t err = AV_ERR_UNKNOWN;
    OH_AVCodecCallback cb;
    cb.onError = &VideoDecoder::OnError;
    cb.onStreamChanged = &VideoDecoder::OnStreamChanged;
    cb.onNeedInputBuffer = &VideoDecoder::OnNeedInputBuffer;
    cb.onNewOutputBuffer = &VideoDecoder::OnNewOutputBuffer;
    err = OH_VideoDecoder_RegisterCallback(codec_, cb, this);

    if (err == AV_ERR_OK) {
        OH_AVFormat *format = OH_AVFormat_Create();
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
        // Surface 模式必须显式声明像素格式为 SURFACE_FORMAT。
        // 不声明的话系统会按默认的 YUVI420(平面) 去协商，而硬件解码器实际写出的是
        // NV12(半平面交错) —— 色度平面被按错误布局解读，整幅画面会泛绿(连灰色都被染色)。
        // 这与 FFmpeg 官方 OpenHarmony 解码器 ohdec 的做法一致:
        //   output_to_window ? AV_PIXEL_FORMAT_SURFACE_FORMAT : AV_PIXEL_FORMAT_NV12
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_SURFACE_FORMAT);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1); // 低时延解码，平台不支持时静默回退
        err = OH_VideoDecoder_Configure(codec_, format);
        OH_AVFormat_Destroy(format);
    }
    if (err == AV_ERR_OK) {
        err = OH_VideoDecoder_SetSurface(codec_, window); // Surface 模式：输出直送显示
    }
    if (err == AV_ERR_OK) {
        err = OH_VideoDecoder_Start(codec_);
    }
    if (err != AV_ERR_OK) {
        LOGE("decoder setup failed, err=%{public}d", err);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
        running_ = false;
        return false;
    }

    startTs_ = std::chrono::steady_clock::now();
    pump_ = std::thread(&VideoDecoder::PumpLoop, this);
    LOG("decoder started, %{public}dx%{public}d", width, height);
    return true;
}

void VideoDecoder::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    cv_.notify_all();
    if (pump_.joinable()) {
        pump_.join();
    }
    if (codec_ != nullptr) {
        OH_VideoDecoder_Stop(codec_);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    std::queue<uint32_t> emptyIdx;
    std::queue<OH_AVBuffer *> emptyBuf;
    std::queue<std::vector<uint8_t>> emptyFrames;
    inIndex_.swap(emptyIdx);
    inBuf_.swap(emptyBuf);
    frames_.swap(emptyFrames);
    LOG("decoder stopped, decoded=%{public}llu",
        static_cast<unsigned long long>(framesDecoded.load()));
}

void VideoDecoder::Feed(const uint8_t *data, size_t len)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (frames_.size() >= MAX_QUEUED_FRAMES) {
        frames_.pop(); // 丢弃最旧帧，防止延迟累积
    }
    frames_.emplace(data, data + len);
    cv_.notify_all();
}

void VideoDecoder::OnError(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    auto *self = static_cast<VideoDecoder *>(userData);
    self->lastError.store(static_cast<uint32_t>(errorCode));
    LOGE("decoder error, code=%{public}d", errorCode);
}

void VideoDecoder::OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
    // 分辨率变化：Surface 模式下显示侧自动缩放，无需处理
}

void VideoDecoder::OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    auto *self = static_cast<VideoDecoder *>(userData);
    {
        std::lock_guard<std::mutex> lock(self->mtx_);
        self->inIndex_.push(index);
        self->inBuf_.push(buffer);
    }
    self->cv_.notify_all();
}

void VideoDecoder::OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    auto *self = static_cast<VideoDecoder *>(userData);
    OH_VideoDecoder_RenderOutputBuffer(codec, index); // 立即送显，最低延迟
    self->framesDecoded.fetch_add(1, std::memory_order_relaxed);
}

void VideoDecoder::PumpLoop()
{
    std::unique_lock<std::mutex> lock(mtx_);
    while (true) {
        cv_.wait(lock, [this] { return !running_.load() || (!inIndex_.empty() && !frames_.empty()); });
        if (!running_.load()) {
            break;
        }
        uint32_t index = inIndex_.front();
        inIndex_.pop();
        OH_AVBuffer *buffer = inBuf_.front();
        inBuf_.pop();
        std::vector<uint8_t> frame = std::move(frames_.front());
        frames_.pop();
        lock.unlock();

        int64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - startTs_)
                            .count();
        OH_AVCodecBufferAttr attr;
        attr.pts = ptsUs;
        attr.size = static_cast<int32_t>(frame.size());
        attr.offset = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        if (OH_AVBuffer_GetCapacity(buffer) >= static_cast<int32_t>(frame.size())) {
            memcpy(OH_AVBuffer_GetAddr(buffer), frame.data(), frame.size());
            OH_AVBuffer_SetBufferAttr(buffer, &attr);
        } else {
            attr.size = 0; // 帧超容量（理论不发生），退回空 buffer 丢弃此帧
            OH_AVBuffer_SetBufferAttr(buffer, &attr);
            LOGE("frame too large, dropped, size=%{public}lu", static_cast<unsigned long>(frame.size()));
        }
        OH_VideoDecoder_PushInputBuffer(codec_, index);

        lock.lock();
    }
}
