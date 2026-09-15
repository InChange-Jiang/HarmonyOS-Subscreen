#include "net_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr uint32_t MAX_PACKET_SIZE = 4 * 1024 * 1024; // 单帧上限，超出视为协议错误断开
constexpr size_t READ_CHUNK = 64 * 1024;
constexpr int POLL_TIMEOUT_MS = 500;
} // namespace

NetServer::~NetServer()
{
    Stop();
}

bool NetServer::Start(uint16_t port, PacketHandler onPacket, StateHandler onState)
{
    if (running_.exchange(true)) {
        return false; // 已在运行
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        running_ = false;
        return false;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有接口：hdcd 经 fport 转发的连接可达
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 1) != 0) {
        ::close(fd);
        running_ = false;
        return false;
    }
    listenFd_ = fd;

    if (::pipe(stopPipe_) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        running_ = false;
        return false;
    }

    port_ = port;
    onPacket_ = std::move(onPacket);
    onState_ = std::move(onState);
    thread_ = std::thread(&NetServer::Run, this);
    return true;
}

void NetServer::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (stopPipe_[1] >= 0) {
        char c = 1;
        ssize_t unused = ::write(stopPipe_[1], &c, 1);
        (void)unused;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    for (int &fd : stopPipe_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

void NetServer::Run()
{
    while (running_.load()) {
        struct pollfd fds[2];
        fds[0].fd = listenFd_;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = stopPipe_[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        int ret = ::poll(fds, 2, POLL_TIMEOUT_MS);
        if (!running_.load()) {
            break;
        }
        if (ret <= 0) {
            continue;
        }
        if (fds[1].revents & POLLIN) {
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr *>(&peer), &peerLen);
        if (clientFd < 0) {
            continue;
        }
        // 关掉 Nagle: 投屏流是"一帧一个小包"的实时流, 攒包会白白增加延迟。
        int one = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (onState_) {
            onState_(true);
        }
        ClientLoop(clientFd);
        ::close(clientFd);
        if (onState_) {
            onState_(false);
        }
    }
}

void NetServer::ClientLoop(int clientFd)
{
    std::vector<uint8_t> buf;
    buf.reserve(READ_CHUNK * 2);
    uint8_t chunk[READ_CHUNK];

    while (running_.load()) {
        struct pollfd fds[2];
        fds[0].fd = clientFd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = stopPipe_[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        int ret = ::poll(fds, 2, POLL_TIMEOUT_MS);
        if (!running_.load()) {
            break;
        }
        if (ret <= 0) {
            continue;
        }
        if (fds[1].revents & POLLIN) {
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        ssize_t n = ::recv(clientFd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break; // 对端关闭或错误
        }
        buf.insert(buf.end(), chunk, chunk + n);

        size_t pos = 0;
        while (pos + 4 <= buf.size()) {
            uint32_t len = (static_cast<uint32_t>(buf[pos]) << 24) | (static_cast<uint32_t>(buf[pos + 1]) << 16) |
                           (static_cast<uint32_t>(buf[pos + 2]) << 8) | static_cast<uint32_t>(buf[pos + 3]);
            if (len > MAX_PACKET_SIZE) {
                return; // 协议错误，断开等待重连
            }
            if (pos + 4 + len > buf.size()) {
                break; // 数据不完整，继续收
            }
            if (onPacket_) {
                onPacket_(buf.data() + pos + 4, len);
            }
            pos += 4 + len;
        }
        if (pos > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(pos));
        }
        if (buf.size() > MAX_PACKET_SIZE + 4) {
            return; // 缓冲失控保护
        }
    }
}
