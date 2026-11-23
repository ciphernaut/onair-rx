// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the polled modes' run and table. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The receivers of these modes put nothing on the automation bus while they decode; what they know is in a snapshot
// (telemetry()) the session reads. So the run only feeds and polls: the session decides what is new, and the run writes it
// with the members every line has. A mode is a row: its name, its event, how often it is polled, how its session is made
// (poll_<mode>.cpp), its hub channel (the cell modes hand their frames to OnAir's Wireshark hub) and whether it needs --freq.
#include "poll.h"
#include "hub_tap.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace onair_rx {

namespace wire = dect2::wire;

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
    // what happens to be there. So the blocks, every t_sample and where each poll lands depend on the input alone, and a
    // replay gives the same lines as a live run. The polls are at the multiples of the interval from sample 0, each after the
    // block that reaches one, so they do not drift.
    constexpr size_t kBlock = kPollBlock;
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
