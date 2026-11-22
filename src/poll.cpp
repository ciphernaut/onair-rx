// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes' run and table. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The receivers of these modes publish nothing while they decode; what they know is in a snapshot (telemetry()) the session
// reads. So the run only feeds and polls: the session decides what is new, and the run writes it with the members every line
// has. A mode is a row: its name, its event, how often it is polled and how its session is made (poll_<mode>.cpp).
#include "poll.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace onair_rx {

const PollMode kPollModes[] = {
    {"rds", "rds.group", 0, openRds},
    {"dmr", "voice.call", 0.25, openDmr},   // the voice receivers publish their snapshot every 0.25 s of signal
    {"p25", "voice.call", 0.25, openP25},
    {"m17", "voice.call", 0.25, openM17},
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

    // Every block is a whole 16384 samples (only the last may be short), however the input arrives: a read of a pipe returns
    // what happens to be there. So the blocks, every t_sample and where each poll lands depend on the input alone, and a
    // replay gives the same lines as a live run. The polls are at the multiples of the interval from sample 0, each after the
    // block that reaches one, so they do not drift.
    constexpr size_t kBlock = 1 << 14;
    const uint64_t every = (uint64_t)std::llround(m.pollSec * o.rate);   // 0: every block
    uint64_t fed = 0, first = 0, due = every;
    std::vector<cf32> iq(kBlock), part;
    for (bool eof = false; !eof;) {
        size_t have = 0;
        while (have < kBlock) {
            part.resize(kBlock - have);
            const size_t n = in.read(part);
            if (n == 0) { eof = true; break; }
            std::copy(part.begin(), part.begin() + (long)n, iq.begin() + (long)have);
            have += n;
        }
        if (have == 0) break;
        first = fed;
        s->feed(iq.data(), have);
        fed += have;
        if (fed >= due) {
            poll(false, first);
            if (every) due = (fed / every + 1) * every;
        }
    }
    poll(true, first);   // what the last block completed, also when it ended short of the interval
    fprintf(stderr, "onair-rx: %s: %llu lines\n", m.name, (unsigned long long)lines);
    return 0;
}

} // namespace onair_rx
