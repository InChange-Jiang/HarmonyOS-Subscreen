#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

// 单客户端 TCP 服务器：监听端口，解析「4字节大端长度 + payload」帧协议。
// 同一时刻只服务一个客户端，断开后自动回到监听状态。
class NetServer {
public:
    using PacketHandler = std::function<void(const uint8_t *data, size_t len)>;
    using StateHandler = std::function<void(bool connected)>;

    ~NetServer();

    bool Start(uint16_t port, PacketHandler onPacket, StateHandler onState);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void Run();
    void ClientLoop(int clientFd);

    int listenFd_ = -1;
    int stopPipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    PacketHandler onPacket_;
    StateHandler onState_;
    uint16_t port_ = 0;
};

#endif // NET_SERVER_H
