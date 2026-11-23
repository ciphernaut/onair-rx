// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// A polled mode is one OnAir receiver that publishes nothing (no bus event, no hub packet) and offers a state snapshot
// instead (its telemetry()). One generic run feeds it and, as the input advances by the mode's poll interval, asks the mode's
// session for a line per thing that is new or has changed since the last poll. The blocks and the interval are counted in
// samples, so a replay gives the same lines as a live run. Text in a line goes through Json::utf8.
#pragma once
#include "rx.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace onair_rx {

// One run of a polled mode: a receiver and what has already been written about its state.
struct PollSession {
    virtual ~PollSession() = default;
    virtual void feed(const cf32* x, size_t n) = 0;
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
};
extern const PollMode kPollModes[];
extern const size_t kPollModeCount;

constexpr size_t kPollBlock = 1 << 14;    // runPolled's block, samples
constexpr double kStatusPollSec = 0.25;   // the status modes' (dvbt, video) poll interval

// Runs a polled mode to EOF: blocks of kPollBlock are fed, each one whole however stdin delivers it (only the last may be short); a
// poll runs after each block that reaches a multiple of pollSec * o.rate samples from sample 0 (every block when 0), and once
// more, with last true, at EOF. Each line is written with "event" (the mode's) and
// "t_sample" (the first sample of the block after which its poll ran) in front. The exit status: 2 with "onair-rx: <err>"
// when open fails, before any sample is read.
int runPolled(const PollMode& m, const Options& o, IqInput& in, Output& out);   // the exit status

// the session of each polled mode (poll_<mode>.cpp; the voice modes dmr, p25 and m17 in poll_voice.cpp, the status modes dvbt
// and video in poll_tv.cpp)
std::unique_ptr<PollSession> openRds(const Options& o, std::string* err);
std::unique_ptr<PollSession> openDmr(const Options& o, std::string* err);
std::unique_ptr<PollSession> openP25(const Options& o, std::string* err);
std::unique_ptr<PollSession> openM17(const Options& o, std::string* err);
std::unique_ptr<PollSession> openDvbt(const Options& o, std::string* err);    // poll_tv.cpp, as video
std::unique_ptr<PollSession> openVideo(const Options& o, std::string* err);

} // namespace onair_rx
