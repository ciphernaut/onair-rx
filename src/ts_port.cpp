// SPDX-License-Identifier: GPL-3.0-or-later
#include "ts_port.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace onair_rx {
namespace {
using Bytes = std::array<uint8_t, 188>;
Bytes mark(uint8_t kind, uint64_t value) {
    Bytes bytes;
    bytes.fill(0xff);
    bytes[0] = 0x47;
    bytes[1] = 0x1f;
    bytes[3] = 0x10;
    std::memcpy(bytes.data() + 4, "ZETATSMK", 8);
    bytes[12] = 1;
    bytes[13] = kind;
    for (int i = 21; i >= 14; --i) { bytes[i] = uint8_t(value); value >>= 8; }
    return bytes;
}
}

bool TsPort::open(int port, double rate, size_t capacity, std::string& err) {
    rate_ = rate;
    capacity_ = capacity;
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (listener_ < 0 || bind(listener_, (sockaddr*)&addr, sizeof addr) || listen(listener_, 1)) {
        err = strerror(errno);
        return false;
    }
    writer_ = std::thread(&TsPort::run, this);
    return true;
}

TsPort::~TsPort() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (writer_.joinable()) writer_.join();
    if (listener_ >= 0) close(listener_);
}

void TsPort::gap() {
    std::lock_guard lock(mutex_);
    gap_ = true;
}

void TsPort::packets(const uint8_t* data, size_t count, uint64_t sample) {
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    for (size_t i = 0; i < count; ++i) {
        while (!queue_.empty() && (capacity_ ? queue_.size() >= capacity_ :
                                   sample - queue_.front().sample >= 2 * rate_)) {
            queue_.pop_front();
            ++drops_;
        }
        auto& packet = queue_.emplace_back();
        std::memcpy(packet.bytes.data(), data + i * 188, 188);
        packet.sample = sample;
        packet.gap = gap_;
        gap_ = false;
    }
    ready_.notify_one();
}

void TsPort::run() {
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
        }
        pollfd fd{listener_, POLLIN, 0};
        if (poll(&fd, 1, 50) <= 0) continue;
        int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        timeval timeout{0, 200000};
        if (!setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)) serve(client);
        close(client);
        std::lock_guard lock(mutex_);
        connected_ = false;
        queue_.clear();
        drops_ = 0;
    }
}

void TsPort::serve(int client) {
    {
        std::lock_guard lock(mutex_);
        connected_ = true;
    }
    uint64_t last = 0;
    bool anchor = true;
    std::vector<Bytes> batch;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, std::chrono::milliseconds(50), [&]{ return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) return;
            batch.clear();
            if (drops_) {
                batch.push_back(mark(2, drops_));
                drops_ = 0;
                anchor = true;
            }
            while (!queue_.empty() && batch.size() < 64) {
                const auto& packet = queue_.front();
                if (packet.gap) { batch.push_back(mark(2, 0)); anchor = true; }
                if (anchor || packet.sample - last >= rate_ / 4) {
                    batch.push_back(mark(1, packet.sample));
                    anchor = false;
                    last = packet.sample;
                }
                batch.push_back(packet.bytes);
                queue_.pop_front();
            }
        }
        pollfd fd{client, POLLRDHUP, 0};
        if (poll(&fd, 1, 0) > 0) return;
        const auto* data = reinterpret_cast<const uint8_t*>(batch.data());
        size_t left = batch.size() * 188;
        while (left) {
            ssize_t n = send(client, data, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            data += n;
            left -= n;
        }
    }
}
}
