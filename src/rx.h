// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: one OnAir receiver on IQ from stdin. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The pieces every mode shares: the options, the IQ input, the outputs (JSON lines on stdout, the pcap port) and the mode table.
#pragma once
#include "dect2/common/ring.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace onair_rx {

using dect2::cf32;

struct Options {
    std::string mode;
    std::string format = "cf32";   // cf32 or cs16, interleaved little endian
    double rate = 0;               // input sample rate, Hz
    double freq = 0;               // --freq: the radio frequency at 0 Hz of the input, Hz (0 when not given)
    int bandwidthMhz = 0;          // --bandwidth: dvbt's channel width, MHz (8, 7, 6 or 5; 0 when not given: each is tried)
    int pcapPort = 0;              // 0: no pcap port
    int tsPort = 0;
    size_t tsBuffer = 0;  // whole packets, or two seconds at observed rate
    // the options only some modes take (main.cpp's kOnlySome says which; another mode refuses each)
    double width = 0;              // --width HZ: the job's band, Hz (0 when not given)
    std::string plan;              // --plan NAME: lorawan's channel plan (empty: the one with most channels in the input)
    std::string region;            // --region NAME: the receiver's region table (empty: the receiver's own choice)
    bool drop = false;             // --drop: a receiver with a worker skips the input that comes while it is busy, as it does live
};

// Reads stdin in blocks and converts to complex float; a sample cut by EOF is dropped.
class IqInput {
public:
    explicit IqInput(const std::string& format) : cs16_(format == "cs16") {}
    // Fills out with the next samples (at most out.size()); 0 at EOF.
    size_t read(std::vector<cf32>& out);
    uint64_t samples() const { return samples_; }
    int64_t startUsec() const { return startUsec_; }   // wall clock when the first sample arrived (0 before)
    int error() const { return error_; }               // the errno of a failed read (0: none; the read loop then saw EOF)
private:
    bool cs16_;
    std::vector<uint8_t> buf_;
    size_t have_ = 0;              // bytes in buf_ not yet converted
    uint64_t samples_ = 0;
    int64_t startUsec_ = 0;
    int error_ = 0;
};

// The time of a pcap record: its signal time (seconds after the first sample) on the wall clock of the first sample, moved on
// when the input falls more than 100 ms behind real time. One per run: the move stays for the records after it.
class RecordClock {
public:
    int64_t at(const IqInput& in, double signalSec) {
        const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!anchorUsec_) anchorUsec_ = in.startUsec();
        int64_t t = anchorUsec_ + (int64_t)std::llround(signalSec * 1e6);
        if (t < now - 100000) { anchorUsec_ += now - t; t = now; }
        return t;
    }
private:
    int64_t anchorUsec_ = 0;   // wall clock of signal time 0
};

// A local TCP port serving one client at a time a classic pcap stream: the file header, then one record per packet.
// Packets wait in a bounded queue (the oldest go first when it is full), so a client that connects late still gets
// the recent ones, and nothing grows without bound when no client ever comes (what is queued at the end with no client
// is counted as dropped). A client that hangs up is dropped and the
// next one gets a new header.
class PcapPort {
public:
    ~PcapPort();
    bool listen(int port, uint32_t linktype, std::string& err);   // 127.0.0.1 only
    void packet(int64_t unixUsec, const uint8_t* p, size_t n);   // never blocks
    void finish();   // write what is queued to a connected client (a send times out after 2 s), then close
    uint64_t sent() const { return sent_; }
    uint64_t dropped() const { return dropped_; }
private:
    void run();
    static constexpr size_t kMaxQueued = 8192;
    int lfd_ = -1, cfd_ = -1;
    std::vector<uint8_t> header_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> q_;
    bool stop_ = false;
    int64_t lastUsec_ = 0;
    std::atomic<uint64_t> sent_{0}, dropped_{0};
    std::thread th_;
};

// What a mode writes: one JSON object per line on stdout (flushed per line), and packets to the pcap port when there is one.
class Output {
public:
    explicit Output(PcapPort* pcap) : pcap_(pcap) {}
    void line(const std::string& json);   // exits the program when stdout is gone
    void packet(int64_t unixUsec, const uint8_t* p, size_t n) { if (pcap_) pcap_->packet(unixUsec, p, n); }
private:
    PcapPort* pcap_;
};

// A JSON object built member by member; a member is only added when its value is known. Strings are written byte by byte: a
// quote, a backslash, a control character and a byte above 0x7e are escaped (such a byte as the code point of its value).
class Json {
public:
    Json& str(const char* k, const std::string& v);
    // Text a receiver hands over in UTF-8 (the polled modes' ps, rt, ...): written as it is when v is valid UTF-8, escaping only
    // a quote, a backslash and a control character (0x7f too); otherwise as str() writes it, so a line is always valid JSON.
    Json& utf8(const char* k, const std::string& v);
    Json& num(const char* k, long long v);
    Json& num(const char* k, double v, int decimals);
    Json& flag(const char* k, bool v);
    Json& arr(const char* k, const std::vector<std::string>& v);   // an array of strings
    Json& raw(const char* k, const std::string& json);             // a value already written as JSON
    Json& omit(const char* k) { keys_.emplace_back(k); return *this; }   // k is left out: has(k) is then true, nothing is written
    bool has(const std::string& k) const;                          // a member of that name is there (or omitted)
    std::string done() const { return s_ + "}"; }
private:
    void key(const char* k);
    std::string s_ = "{";
    std::vector<std::string> keys_;
};

// The mode table: a mode's name, the pcap link type of its packets and how to run it. run returns the exit status.
struct Mode {
    const char* name;
    uint32_t linktype;
    int (*run)(const Options& o, IqInput& in, Output& out);
};

int runAdsb(const Options& o, IqInput& in, Output& out);   // the bus modes are in bus.h

} // namespace onair_rx
