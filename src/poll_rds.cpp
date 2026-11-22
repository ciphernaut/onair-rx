// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the rds mode. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's FmReceiver decodes the station at 0 Hz of the input, its RDS among the rest. Its group tap hands over every raw group
// from inside feed(), with each block's state (0 good, 1 corrected, 2 bad); the session queues one line per group and poll()
// hands them over, so the mode is polled after every block. A line holds what the group itself says (the PI, the group type,
// the programme type, the traffic programme flag), and the programme service name and the radiotext as OnAir's snapshot has
// them at that group, each left out until OnAir has it complete.
#include "poll.h"
#include "dect2/fm/fm_rx.h"
#include <cstdio>

namespace onair_rx {

using namespace dect2;

namespace {

constexpr double kRdsMinRate = 500e3;   // FmReceiver::ready() is false below it (fm_rx.h: "any input rate from 500 kHz up")

class RdsSession : public PollSession {
public:
    // setSilent before configure, which otherwise opens a sound device; the station finder off: the station is the one at
    // the centre, as the node's channel has it, however weak
    bool open(const Options& o, std::string* err) {
        rx_.setSilent(true);
        rx_.configure(o.rate);
        if (!rx_.ready()) {
            char b[160];
            snprintf(b, sizeof b, "%.0f Hz is too low for rds (at least %.0f Hz)", o.rate, kRdsMinRate);
            *err = b;
            return false;
        }
        rx_.setStationFinder(false);
        rx_.setRdsGroupTap([this](const uint16_t b[4], const uint8_t e[4]) { group(b, e); });   // from feed(), this thread
        return true;
    }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void poll(bool, std::vector<Json>& lines) override {
        for (Json& j : queued_) lines.push_back(std::move(j));
        queued_.clear();
    }

private:
    // A group gives a line only when its block B (the type, PTY and TP) and its PI (block A, or C' in a version B group, as
    // OnAir reads it) came with a right checkword. A corrected block is a guess until it repeats (OnAir uses one only then),
    // and the tap hands it over before that is known, so it is not taken: the station's groups repeat many times a second.
    void group(const uint16_t b[4], const uint8_t e[4]) {
        if (e[1] != 0) return;
        const bool verB = (b[1] >> 11) & 1;
        int pi = -1;
        if (e[0] == 0) pi = b[0];
        else if (verB && e[2] == 0) pi = b[2];
        if (pi < 0) return;
        if (rx_.telemetry(tel_, seq_)) seq_ = tel_.seq;   // the snapshot is published every 0.1 s; between, the last one stands

        char hex[8], type[8];
        snprintf(hex, sizeof hex, "%04X", pi);
        snprintf(type, sizeof type, "%d%c", b[1] >> 12, verB ? 'B' : 'A');
        Json j;
        j.str("pi", hex).str("group", type).num("pty", (long long)((b[1] >> 5) & 31)).flag("tp", (b[1] >> 10) & 1);
        // OnAir hands both over in UTF-8, read from the station's character set (rdsText)
        if (!tel_.psName.empty()) j.utf8("ps", tel_.psName);   // OnAir sets it only once all four segments came
        if (tel_.radioTextComplete && !tel_.radioText.empty()) j.utf8("rt", tel_.radioText);
        queued_.push_back(std::move(j));
    }

    FmReceiver rx_;
    FmTelemetry tel_;
    uint64_t seq_ = 0;
    std::vector<Json> queued_;
};

} // namespace

std::unique_ptr<PollSession> openRds(const Options& o, std::string* err) {
    auto s = std::make_unique<RdsSession>();
    if (!s->open(o, err)) return nullptr;
    return s;
}

} // namespace onair_rx
