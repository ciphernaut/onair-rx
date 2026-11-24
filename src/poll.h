// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// A polled mode is one OnAir receiver that puts nothing on the automation bus and offers a state snapshot instead (its
// telemetry()). One generic run feeds it and, as the input advances by the mode's poll interval, asks the mode's session for a
// line per thing that is new or has changed since the last poll. The blocks and the interval are counted in samples, so a
// replay gives the same lines as a live run. Text in a line goes through Json::utf8. A mode whose receiver also hands frames to
// OnAir's Wireshark hub (tetra, gsmr, lte, dvbs) has a hub channel: with a pcap port the run writes them as records.
#pragma once
#include "rx.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace onair_rx {

// One run of a polled mode: a receiver and what has already been written about its state.
struct PollSession {
    virtual ~PollSession() = default;
    // A receiver that decodes on a worker of its own and skips the input that comes while it is busy (tetra, gsmr, lte) is waited
    // for: feed returns only once it is no longer busy, so that it never skips input (with --drop, it skips as it does live).
    // dvbs's receiver waits for its own decoder threads instead (its blocking switch), and its session for what is queued at
    // each poll.
    virtual void feed(const cf32* x, size_t n) = 0;
    // Under --drop: n samples of the input were dropped by the read-ahead (its queue was full) before the block fed next. A
    // cell mode's session drops the part of a stretch it holds, so that the receiver never decodes a stretch with a hole in it
    virtual void gap(uint64_t n) { (void)n; }
    // The lines for what is new or changed since the last poll. last: the input has ended.
    // A line has neither "event" nor "t_sample": runPolled adds both.
    virtual void poll(bool last, std::vector<Json>& lines) = 0;
};
struct PollMode {
    const char* name;      // "rds"
    const char* event;     // "rds.group"
    double pollSec;        // seconds of input between polls; 0: after every block
    // makes the session for o; nullptr with *err set when o does not suit the mode (the rate is too low)
    std::unique_ptr<PollSession> (*open)(const Options& o, std::string* err);
    int chan = -1;            // a dect2::wire::Chan, or -1: no packets (--pcap-port is refused)
    bool needsFreq = false;   // the receiver is told the radio frequency of the input: --freq is required
    // seconds of stdin read ahead (runPolled's read-ahead thread), at least the longest wait of a session that waits for its
    // receiver; 0: two blocks, for a session that never waits. Under --drop this or kDropReadAheadSec, whichever is longer,
    // so that a writer's bursts fit; never more than 256 MB
    double readAheadSec = 0;
};
extern const PollMode kPollModes[];
extern const size_t kPollModeCount;

constexpr size_t kPollBlock = 1 << 14;    // runPolled's block, samples
constexpr double kStatusPollSec = 0.25;   // the status modes' (dvbt, video, dvbs) poll interval
constexpr double kStatusMostSec = 2;      // a status line at least this often, seconds of input
// The least read-ahead under --drop, seconds of input: a writer that hands over its input in bursts (ZetaSDR's capture_decode
// writes 50 ms at a time) is never cut short by a full queue while the session keeps up on average
constexpr double kDropReadAheadSec = 0.25;

// When a status line is due (the status modes: dvbt and video in poll_tv.cpp, dvbs in poll_dvbs.cpp). The first waits for first
// lock or kStatusMostSec of input, whichever comes first (the poll of the block that reaches it), as the row's own decoders'
// first event does: the node judges a job by its first event, so a signal that locks by then is reported locked. Input that
// ends before either has its first line from the poll at the end. After it, a poll comes every kStatusPollSec of input, after
// the block of kPollBlock that reaches it; so a line is written at the last poll before the gap since the one before could
// pass kStatusMostSec, and lines are never further apart than that, whatever block a poll lands on.
class Cadence {
public:
    explicit Cadence(double rate)
        : most_((uint64_t)std::llround(kStatusMostSec * rate)), slack_((uint64_t)std::llround(kStatusPollSec * rate) + kPollBlock) {}
    void fed(size_t n) { now_ += n; }
    // j, the line, is added to lines when it is due: the first once locked, at kStatusMostSec or at the end of input (last),
    // so input that ends sooner without a lock still gives its one line (empty input gives none); then on a change of key
    // (the state: the line's members without the measured ones) or when late
    void offer(bool locked, bool last, const std::string& key, Json&& j, std::vector<Json>& lines) {
        const bool late = now_ + slack_ > at_ + most_;   // the next poll could come more than kStatusMostSec after the last line
        const bool ended = last && now_ > 0;
        if (written_ ? key == key_ && !late : !locked && !ended && now_ < most_) return;
        at_ = now_;
        key_ = key;
        written_ = true;
        lines.push_back(std::move(j));
    }
private:
    uint64_t most_, slack_, now_ = 0, at_ = 0;   // at_: the input fed when the last line was written (0 before the first)
    std::string key_;
    bool written_ = false;
};

// Runs a polled mode to EOF: blocks of kPollBlock are fed, each one whole however stdin delivers it (only the last may be short); a
// poll runs after each block that reaches a multiple of pollSec * o.rate samples from sample 0 (every block when 0), and once
// more, with last true, at EOF. Each line is written with "event" (the mode's) and
// "t_sample" (the first sample of the block after which its poll ran) in front. With a hub channel and a pcap port, the hub's
// packets are drained after every block and after the last poll, each one record (hubRecord) at the drained block's first
// sample. The exit status: 2 with "onair-rx: <err>" when open fails, before any sample is read.
int runPolled(const PollMode& m, const Options& o, IqInput& in, Output& out);   // the exit status

// the session of each polled mode (poll_<mode>.cpp; the voice modes dmr, p25 and m17 in poll_voice.cpp, the status modes dvbt
// and video in poll_tv.cpp, the cell modes tetra, gsmr and lte in poll_cell.cpp)
std::unique_ptr<PollSession> openRds(const Options& o, std::string* err);
std::unique_ptr<PollSession> openDmr(const Options& o, std::string* err);
std::unique_ptr<PollSession> openP25(const Options& o, std::string* err);
std::unique_ptr<PollSession> openM17(const Options& o, std::string* err);
std::unique_ptr<PollSession> openDvbt(const Options& o, std::string* err);    // poll_tv.cpp, as video
std::unique_ptr<PollSession> openVideo(const Options& o, std::string* err);
std::unique_ptr<PollSession> openTetra(const Options& o, std::string* err);   // poll_cell.cpp, as gsmr
std::unique_ptr<PollSession> openGsmr(const Options& o, std::string* err);
std::unique_ptr<PollSession> openLte(const Options& o, std::string* err);
std::unique_ptr<PollSession> openDvbs(const Options& o, std::string* err);    // poll_dvbs.cpp

} // namespace onair_rx
