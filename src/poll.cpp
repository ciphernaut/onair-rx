// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes' run and table. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The receivers of these modes put nothing on the automation bus while they decode; what they know is in a snapshot
// (telemetry()) the session reads. So the run only feeds and polls: the session decides what is new, and the run writes it
// with the members every line has. A mode is a row: its name, its event, how often it is polled, how its session is made
// (poll_<mode>.cpp), its hub channel (the cell modes and dvbs hand their frames to OnAir's Wireshark hub), whether it needs
// --freq and how far stdin is read ahead. stdin is read on a thread of its own (ReadAhead), so that a session waiting for its
// receiver never holds up the writer.
#include "poll.h"
#include "hub_tap.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace onair_rx {

namespace wire = dect2::wire;

namespace {

// stdin read ahead on a thread of its own into a bounded queue of whole blocks: while a session waits for its receiver's
// worker (tetra, gsmr), stdin is still drained, so a writer feeding at real time is not held up for the decoding. The reader
// gathers what stdin delivers into blocks of kPollBlock (the last may be short), however small its reads, so the queue holds
// whole blocks and the read loop takes them as they are. The queue holds `blocks` of them (the mode's readAheadSec; two
// blocks for a mode whose session does not wait); when it is full the reader waits, so the writer is held up then
// (back-pressure, as without the thread) and no sample is dropped. Under --drop it never waits: a block that finds the
// queue full is dropped and counted, and the block after it says how many samples went before it, so that the sample count
// (t_sample) stays the index on the input. EOF and a read error (IqInput::error(), read by the caller once the run is
// over) end the blocks.
class ReadAhead {
public:
    ReadAhead(IqInput& in, size_t blocks, bool drop) : in_(in), cap_(std::max<size_t>(1, blocks)), drop_(drop), th_([this] { run(); }) {}
    ~ReadAhead() {   // the run reads to EOF, so the reader has ended; it is only told to stop should it still wait for room
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        th_.join();
    }
    ReadAhead(const ReadAhead&) = delete;
    ReadAhead& operator=(const ReadAhead&) = delete;
    // The next block into out, and in dropped the samples dropped before it; false at the end of input (dropped: those
    // dropped after the last block)
    bool next(std::vector<cf32>& out, uint64_t& dropped) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !q_.empty() || eof_; });
        if (q_.empty()) { dropped = droppedSince_; droppedSince_ = 0; return false; }
        out.swap(q_.front().samples);
        dropped = q_.front().droppedBefore;
        q_.pop_front();
        cv_.notify_all();
        return true;
    }
    uint64_t dropped() const { std::lock_guard<std::mutex> lk(mu_); return droppedTotal_; }   // all samples dropped so far

private:
    struct Block {
        std::vector<cf32> samples;
        uint64_t droppedBefore;   // samples dropped between the block before it and this one
    };
    void run() {
        std::vector<cf32> block(kPollBlock), part;
        size_t have = 0;
        for (;;) {
            part.resize(kPollBlock - have);
            const size_t n = in_.read(part);   // outside the lock: what stdin has, up to the rest of the block
            std::copy(part.begin(), part.begin() + (long)n, block.begin() + (long)have);
            have += n;
            if (n > 0 && have < kPollBlock) continue;
            if (have > 0) {   // a whole block, or the last one
                block.resize(have);
                if (!put(block)) return;
                block.assign(kPollBlock, cf32());
                have = 0;
            }
            if (n == 0) {
                std::lock_guard<std::mutex> lk(mu_);
                eof_ = true;
                cv_.notify_all();
                return;
            }
        }
    }
    bool put(std::vector<cf32>& block) {   // false: told to stop
        std::unique_lock<std::mutex> lk(mu_);
        if (drop_ && q_.size() >= cap_) {
            droppedSince_ += block.size();
            droppedTotal_ += block.size();
            return true;
        }
        cv_.wait(lk, [&] { return stop_ || q_.size() < cap_; });
        if (stop_) return false;
        q_.push_back({std::move(block), droppedSince_});
        droppedSince_ = 0;
        cv_.notify_all();
        return true;
    }
    IqInput& in_;
    const size_t cap_;     // blocks
    const bool drop_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Block> q_;
    uint64_t droppedSince_ = 0, droppedTotal_ = 0;
    bool eof_ = false, stop_ = false;
    std::thread th_;       // last: it starts once the rest is made
};

// The read-ahead's blocks for a mode: two, or the mode's readAheadSec at the rate when its session waits for its receiver (and
// not under --drop, where it does not); never more than kReadAheadMaxBytes, whatever the rate
constexpr size_t kReadAheadMaxBytes = (size_t)256 << 20;
size_t readAheadBlocks(const PollMode& m, const Options& o) {
    size_t blocks = 2;
    if (m.readAheadSec > 0 && !o.drop) blocks = std::max(blocks, (size_t)std::ceil(m.readAheadSec * o.rate / (double)kPollBlock));
    return std::min(blocks, kReadAheadMaxBytes / (kPollBlock * sizeof(cf32)));
}

} // namespace

const PollMode kPollModes[] = {
    {"rds", "rds.group", 0, openRds},
    {"dmr", "voice.call", 0.25, openDmr},   // the voice receivers publish their snapshot every 0.25 s of signal
    {"p25", "voice.call", 0.25, openP25},
    {"m17", "voice.call", 0.25, openM17},
    {"dvbt", "dvbt.status", kStatusPollSec, openDvbt},   // a status line on a change and at least every 2 s (poll_tv.cpp)
    {"video", "video.status", kStatusPollSec, openVideo},
    // a cell line when a cell is first read and when it changes (poll_cell.cpp); the receivers decode a stretch of 2.3 s and of
    // 2.2 s at a time, so the poll interval only bounds how long after a stretch its lines come. Their sessions wait for the
    // decoding of a stretch, so stdin is read ahead by two stretches (TetraReceiver::kCaptureSec, gsmr_rx.cpp's kBlockSec)
    {"tetra", "tetra.cell", 0.25, openTetra, wire::Tetra, true, 2 * 2.3},
    {"gsmr", "gsmr.cell", 0.25, openGsmr, wire::GsmR, true, 2 * 2.2},
    // LTE decodes a block of 0.2 s at a time (cell_rx.cpp's kBlockSec), so stdin is read ahead by two of them, as for the other
    // cell modes. It decodes more slowly than the input comes at every rate (on the build host 0.8 s for a block at 4.6 Msps,
    // 2.8 s at 20 Msps), so no read-ahead would keep a writer at real time from being held up without --drop: a larger one
    // would only put the stall off (README, "lte")
    {"lte", "lte.cell", 0.25, openLte, wire::Lte, true, 2 * 0.2},
    // a status line as dvbt's (poll_tv.cpp); the IP datagrams on the hub. Its session waits for the receiver's decoder threads
    // (the blocking switch) and, at each poll, for what is queued; with its search and frame hunt, which run in feed, that held
    // a block up to 111 ms on the build host (README, "dvbs"): stdin is read ahead by twice that and more
    {"dvbs", "dvbs.status", kStatusPollSec, openDvbs, wire::DvbIp, false, 0.25},
};
const size_t kPollModeCount = sizeof kPollModes / sizeof kPollModes[0];

int runPolled(const PollMode& m, const Options& o, IqInput& in, Output& out) {
    std::string err;
    const std::unique_ptr<PollSession> s = m.open(o, &err);
    if (!s) { fprintf(stderr, "onair-rx: %s\n", err.c_str()); return 2; }

    uint64_t lines = 0;
    std::vector<Json> polled;
    // the poll's lines, each with event and t_sample in front of the session's members (as the bus modes' lines have them)
    auto poll = [&](bool last, uint64_t first) {
        s->poll(last, polled);
        for (const Json& l : polled) {
            Json head;
            head.str("event", m.event).num("t_sample", (long long)first);
            std::string line = head.done(), body = l.done();   // "{...}" both
            line.pop_back();
            if (body.size() > 2) line += "," + body.substr(1);
            else line += "}";
            out.line(line);
            lines++;
        }
        polled.clear();
    };

    // the hub's packets of the mode's channel, collected from whatever thread publishes them (a cell receiver's worker), only
    // with a pcap port: without one the hub's channel stays off and the receiver packs nothing
    const bool records = m.chan >= 0 && o.pcapPort;
    std::unique_ptr<HubTap> tap;
    if (records) tap = std::make_unique<HubTap>(m.chan);
    RecordClock clock;
    uint64_t sent = 0;
    std::vector<std::vector<uint8_t>> packets;
    auto drain = [&](uint64_t first) {   // what arrived since the last drain, as records at first, the drained block's first sample
        if (!tap) return;
        tap->drain(packets);
        if (packets.empty()) return;
        const int64_t t = clock.at(in, (double)first / o.rate);
        for (const auto& p : packets) {
            const std::vector<uint8_t> r = hubRecord(m.chan, p.data(), p.size());
            out.packet(t, r.data(), r.size());
        }
        sent += packets.size();
        packets.clear();
    };

    // Every block is a whole 16384 samples (only the last may be short), however the input arrives: a read of a pipe returns
    // what happens to be there, and the read-ahead gathers it into blocks. So the blocks, every t_sample and where each poll
    // lands depend on the input alone, and a replay gives the same lines as a live run. The polls are at the multiples of the
    // interval from sample 0, each after the block that reaches one, so they do not drift. Samples the read-ahead dropped
    // (--drop) count as input: the next block starts after them.
    const uint64_t every = (uint64_t)std::llround(m.pollSec * o.rate);   // 0: every block
    uint64_t fed = 0, first = 0, due = every, skipped = 0;
    // a test's slow session (test/only.sh, --drop's read-ahead): this long a sleep after each block, in microseconds
    const char* slow = getenv("ONAIR_RX_TEST_BLOCK_DELAY_US");
    const long delayUs = slow ? atol(slow) : 0;
    ReadAhead ahead(in, readAheadBlocks(m, o), o.drop);
    std::vector<cf32> iq;
    while (ahead.next(iq, skipped)) {
        fed += skipped;
        first = fed;
        s->feed(iq.data(), iq.size());
        fed += iq.size();
        drain(first);
        if (fed >= due) {
            poll(false, first);
            if (every) due = (fed / every + 1) * every;
        }
        if (delayUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
    }
    poll(true, first);   // what the last block completed, also when it ended short of the interval
    drain(first);        // and what the last poll waited for
    tap.reset();
    fprintf(stderr, "onair-rx: %s: %llu lines", m.name, (unsigned long long)lines);
    if (records) fprintf(stderr, ", %llu packets", (unsigned long long)sent);
    fprintf(stderr, "\n");
    if (const uint64_t n = ahead.dropped()) fprintf(stderr, "onair-rx: %s: %llu samples dropped\n", m.name, (unsigned long long)n);
    return 0;
}

} // namespace onair_rx
