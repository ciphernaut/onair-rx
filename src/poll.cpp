// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes' run and table. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The receivers of these modes put nothing on the automation bus while they decode; what they know is in a snapshot
// (telemetry()) the session reads. So the run only feeds and polls: the session decides what is new, and the run writes it
// with the members every line has. A mode is a row: its name, its event, how often it is polled, how its session is made
// (poll_<mode>.cpp), its hub channel (the cell modes hand their frames to OnAir's Wireshark hub) and whether it needs --freq.
// stdin is read on a thread of its own (ReadAhead), so that a session waiting for its receiver never holds up the writer.
#include "poll.h"
#include "hub_tap.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace onair_rx {

namespace wire = dect2::wire;

namespace {

// stdin read ahead on a thread of its own into a bounded queue: while a session waits for its receiver's worker (tetra,
// gsmr), stdin is still drained, so a writer feeding at real time is not held up for the decoding. The queue holds
// kReadAheadSec of input at the rate (at least two blocks); when it is full the reader waits, so the writer is held up then
// (back-pressure, as without the thread) and no sample is ever dropped, with --drop too (there the receiver skips, as it
// does live, and the session does not wait, so the queue does not fill). The samples come out in the order they were read;
// EOF and a read error (IqInput::error(), read by the caller once the run is over) end them.
constexpr double kReadAheadSec = 4;
class ReadAhead {
public:
    ReadAhead(IqInput& in, size_t capacity) : in_(in), cap_(capacity), th_([this] { run(); }) {}
    ~ReadAhead() {   // the run reads to EOF, so the reader has ended; it is only told to stop should it still wait for room
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        th_.join();
    }
    ReadAhead(const ReadAhead&) = delete;
    ReadAhead& operator=(const ReadAhead&) = delete;
    // The next samples, at most n, into x, as soon as any are there; 0 at the end of input
    size_t read(cf32* x, size_t n) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !q_.empty() || eof_; });
        if (q_.empty()) return 0;
        const std::vector<cf32>& c = q_.front();
        const size_t k = std::min(n, c.size() - at_);
        std::copy(c.begin() + (long)at_, c.begin() + (long)(at_ + k), x);
        at_ += k;
        queued_ -= k;
        if (at_ == c.size()) { q_.pop_front(); at_ = 0; }
        cv_.notify_all();
        return k;
    }

private:
    void run() {
        std::vector<cf32> buf(kPollBlock);
        for (;;) {
            const size_t n = in_.read(buf);   // outside the lock: what stdin has, up to a block
            std::unique_lock<std::mutex> lk(mu_);
            if (n == 0) { eof_ = true; cv_.notify_all(); return; }
            cv_.wait(lk, [&] { return stop_ || queued_ == 0 || queued_ + n <= cap_; });
            if (stop_) return;
            q_.emplace_back(buf.begin(), buf.begin() + (long)n);
            queued_ += n;
            cv_.notify_all();
        }
    }
    IqInput& in_;
    const size_t cap_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<cf32>> q_;
    size_t at_ = 0;        // samples of q_.front() already taken
    size_t queued_ = 0;    // samples in q_ not yet taken
    bool eof_ = false, stop_ = false;
    std::thread th_;       // last: it starts once the rest is made
};

} // namespace

const PollMode kPollModes[] = {
    {"rds", "rds.group", 0, openRds},
    {"dmr", "voice.call", 0.25, openDmr},   // the voice receivers publish their snapshot every 0.25 s of signal
    {"p25", "voice.call", 0.25, openP25},
    {"m17", "voice.call", 0.25, openM17},
    {"dvbt", "dvbt.status", kStatusPollSec, openDvbt},   // a status line on a change and at least every 2 s (poll_tv.cpp)
    {"video", "video.status", kStatusPollSec, openVideo},
    // a cell line when a cell is first read and when it changes (poll_cell.cpp); the receivers decode a stretch of 2.3 s and of
    // 2.2 s at a time, so the poll interval only bounds how long after a stretch its lines come
    {"tetra", "tetra.cell", 0.25, openTetra, wire::Tetra, true},
    {"gsmr", "gsmr.cell", 0.25, openGsmr, wire::GsmR, true},
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
    // what happens to be there, and the read-ahead hands on what it has. So the blocks, every t_sample and where each poll
    // lands depend on the input alone, and a replay gives the same lines as a live run. The polls are at the multiples of the interval from sample 0, each after the
    // block that reaches one, so they do not drift.
    constexpr size_t kBlock = kPollBlock;
    const uint64_t every = (uint64_t)std::llround(m.pollSec * o.rate);   // 0: every block
    uint64_t fed = 0, first = 0, due = every;
    ReadAhead ahead(in, std::max<size_t>(2 * kBlock, (size_t)std::ceil(kReadAheadSec * o.rate)));
    std::vector<cf32> iq(kBlock);
    for (bool eof = false; !eof;) {
        size_t have = 0;
        while (have < kBlock) {
            const size_t n = ahead.read(iq.data() + have, kBlock - have);
            if (n == 0) { eof = true; break; }
            have += n;
        }
        if (have == 0) break;
        first = fed;
        s->feed(iq.data(), have);
        fed += have;
        drain(first);
        if (fed >= due) {
            poll(false, first);
            if (every) due = (fed / every + 1) * every;
        }
    }
    poll(true, first);   // what the last block completed, also when it ended short of the interval
    drain(first);        // and what the last poll waited for
    tap.reset();
    fprintf(stderr, "onair-rx: %s: %llu lines", m.name, (unsigned long long)lines);
    if (records) fprintf(stderr, ", %llu packets", (unsigned long long)sent);
    fprintf(stderr, "\n");
    return 0;
}

} // namespace onair_rx
