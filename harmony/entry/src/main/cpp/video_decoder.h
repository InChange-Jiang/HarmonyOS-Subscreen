#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <native_window/external_window.h>

struct OH_AVCodec;
struct OH_AVBuffer;
struct OH_AVFormat;

// H.264 硬解码（Surface 模式）：解码输出直送 XComponent Surface，零拷贝渲染。
// 输入帧由 Feed() 投入有界队列（丢旧帧保低延迟），pump 线程写入解码器；
// 输出在 onNewOutputBuffer 回调里立即 RenderOutputBuffer 送显。
class VideoDecoder {
public:
    bool Start(int width, int height, OHNativeWindow *window);
    void Stop();
    void Feed(const uint8_t *data, size_t len);
    bool IsRunning() const { return running_.load(); }

    // 统计：解码输出帧数（任意线程可读）
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint32_t> lastError{0};

private:
    static void OnError(OH_AVCodec *codec, int32_t errorCode, void *userData);
    static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData);
    static void OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);
    static void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);
    void PumpLoop();

    OH_AVCodec *codec_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread pump_;

    // 输入索引队列（解码器回调填充）+ 待解码帧队列（网络线程填充），共用一把锁
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<uint32_t> inIndex_;
    std::queue<OH_AVBuffer *> inBuf_;
    std::queue<std::vector<uint8_t>> frames_;

    std::chrono::steady_clock::time_point startTs_;
};

#endif // VIDEO_DECODER_H
