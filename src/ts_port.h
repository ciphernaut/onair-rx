// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace onair_rx {
class TsPort {
public:
    ~TsPort();
    bool open(int port, double rate, size_t capacity, std::string& err);
    void packets(const uint8_t* data, size_t count, uint64_t sample);
    void gap();
private:
    struct Packet {
        std::array<uint8_t, 188> bytes;
        uint64_t sample;
        bool gap;
    };
    int listener_ = -1;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread writer_;
    std::deque<Packet> queue_;
    double rate_ = 0;
    size_t capacity_ = 0;
    uint64_t drops_ = 0;
    bool gap_ = false, connected_ = false, stopping_ = false;
    void run();
    void serve(int client);
};
}
