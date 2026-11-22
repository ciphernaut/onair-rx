// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes' run and table. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The receivers of these modes publish nothing while they decode; what they know is in a snapshot (telemetry()) the session
// reads. So the run only feeds and polls: the session decides what is new, and the run writes it with the members every line
// has. A mode is a row: its name, its event, how often it is polled and how its session is made (poll_<mode>.cpp).
#include "poll.h"
#include <cmath>
#include <cstdio>

namespace onair_rx {

const PollMode kPollModes[] = {
    {"rds", "rds.group", 0, openRds},
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

    const uint64_t every = (uint64_t)std::llround(m.pollSec * o.rate);   // 0: every block
    uint64_t polledAt = 0, first = 0;
    std::vector<cf32> iq(1 << 14);
    for (size_t n; (n = in.read(iq)) > 0;) {
        first = in.samples() - n;
        s->feed(iq.data(), n);
        if (in.samples() - polledAt >= every) {
            poll(false, first);
            polledAt = in.samples();
        }
    }
    poll(true, first);   // what the last block completed, also when it ended short of the interval
    fprintf(stderr, "onair-rx: %s: %llu lines\n", m.name, (unsigned long long)lines);
    return 0;
}

} // namespace onair_rx
