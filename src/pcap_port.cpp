// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the pcap port. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
// The byte layout is OnAir's own (wire_format.h: classic pcap, little endian, microseconds).
#include "rx.h"
#include "dect2/wirecap/wire_format.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace onair_rx {

namespace wire = dect2::wire;

PcapPort::~PcapPort() {
    finish();
}

bool PcapPort::listen(int port, uint32_t linktype, std::string& err) {
    lfd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd_ < 0) { err = strerror(errno); return false; }
    const int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd_, (sockaddr*)&a, sizeof a) != 0 || ::listen(lfd_, 1) != 0) {
        err = strerror(errno);
        close(lfd_);
        lfd_ = -1;
        return false;
    }
    header_ = wire::pcapFileHeader(linktype);
    th_ = std::thread([this] { run(); });
    return true;
}

void PcapPort::packet(int64_t t, const uint8_t* p, size_t n) {
    std::vector<uint8_t> rec;
    std::lock_guard<std::mutex> lk(mu_);
    if (lfd_ < 0 || stop_) return;
    if (t < lastUsec_) t = lastUsec_;   // records in time order
    lastUsec_ = t;
    wire::appendPcapRecord(rec, t, p, n);
    if (q_.size() >= kMaxQueued) { q_.pop_front(); dropped_++; }
    q_.push_back(std::move(rec));
    cv_.notify_one();
}

namespace {
bool sendAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}
// The client hung up (or sent something, which is read and ignored)
bool hungUp(int fd) {
    pollfd pf{fd, POLLIN | POLLRDHUP, 0};
    if (poll(&pf, 1, 0) <= 0) return false;
    if (pf.revents & (POLLHUP | POLLERR | POLLRDHUP)) return true;
    char junk[256];
    return recv(fd, junk, sizeof junk, MSG_DONTWAIT) == 0;
}
} // namespace

void PcapPort::run() {
    for (;;) {
        if (cfd_ < 0) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_) { dropped_ += q_.size(); q_.clear(); break; }   // nobody is listening: what is queued goes
            }
            pollfd pf{lfd_, POLLIN, 0};
            if (poll(&pf, 1, 100) <= 0) continue;
            const int c = accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            timeval tv{2, 0};   // a client that stops reading is dropped rather than holding the queue
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (!sendAll(c, header_.data(), header_.size())) { close(c); continue; }
            cfd_ = c;
            fprintf(stderr, "onair-rx: a pcap client connected\n");
            continue;
        }
        std::vector<uint8_t> batch;
        size_t records = 0;
        bool stopping;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(200), [this] { return stop_ || !q_.empty(); });
            stopping = stop_;
            for (auto& r : q_) batch.insert(batch.end(), r.begin(), r.end());
            records = q_.size();
            q_.clear();
        }
        bool ok = batch.empty() ? !hungUp(cfd_) : sendAll(cfd_, batch.data(), batch.size());
        if (ok) sent_ += records;
        else {
            dropped_ += records;
            close(cfd_);
            cfd_ = -1;
            fprintf(stderr, "onair-rx: the pcap client went away\n");
        }
        if (stopping && batch.empty()) break;
    }
    if (cfd_ >= 0) { close(cfd_); cfd_ = -1; }
}

void PcapPort::finish() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        cv_.notify_one();
    }
    if (th_.joinable()) th_.join();
    if (lfd_ >= 0) { close(lfd_); lfd_ = -1; }
}

} // namespace onair_rx
