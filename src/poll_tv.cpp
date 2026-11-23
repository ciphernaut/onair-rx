// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the status modes, dvbt and video. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// Neither receiver has a decode to hand over one by one: what it knows is a state (locked or not, and what the signal says
// about itself), in its snapshot. A status mode writes that state as one line: the first at first lock or after 2 s of input,
// whichever comes first; then one whenever the state changes, and one at least every 2 s, so that a watcher always has a
// current one. A measured value (an SNR, a line rate) is written with the state but makes no line of its own: it moves at every
// poll. All of it is counted in samples of input, so a replay gives the same lines as a live run.
//
// dvbt: OnAir's DvbtReceiver, on one channel width at a time. Without --bandwidth it tries 8, 7, 6 and 5 MHz (those the rate
// takes) for 2 s of input each, in turn, until the TPS is decoded, and stays on that width while it is. The transport stream it
// decodes goes to OnAir's demultiplexer (TsDemux) only, for the service list; nothing else takes the packets.
// video: OnAir's AtvReceiver in its FM video mode (analog FPV links, what the node's video row decodes). No picture is taken.
#include "poll.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/tv/ts.h"
#include <cmath>
#include <cstdio>

namespace onair_rx {

using namespace dect2;

namespace {

constexpr double kStatusMostSec = 2;   // a status line at least this often, seconds of input

// When a status line is due. A poll comes every kStatusPollSec of input, after the block of kPollBlock that reaches it; so a
// line is written at the last poll before the gap since the one before could pass kStatusMostSec, and lines are never further
// apart than that, whatever block a poll lands on.
class Cadence {
public:
    explicit Cadence(double rate)
        : most_((uint64_t)std::llround(kStatusMostSec * rate)), slack_((uint64_t)std::llround(kStatusPollSec * rate) + kPollBlock) {}
    void fed(size_t n) { now_ += n; }
    // j, the line, is added to lines when it is due: the first once locked or late, then on a change of key (the state: the
    // line's members without the measured ones) or when late
    void offer(bool locked, const std::string& key, Json&& j, std::vector<Json>& lines) {
        const bool late = now_ + slack_ > at_ + most_;   // the next poll could come more than kStatusMostSec after the last line
        if (written_ ? key == key_ && !late : !locked && !late) return;
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

// --- dvbt

constexpr int kDvbtWidths[] = {8, 7, 6, 5};   // MHz, in the order they are tried (as the row's own program does)
constexpr double kDvbtTrySec = 2;            // seconds of input on a width that does not lock before the next is tried
// the lowest rate DvbtReceiver takes for a width (dvbt_rx.cpp, configure: 7.9 MHz for 8 MHz)
double dvbtFloor(int mhz) { return 7.9e6 * mhz / 8.0; }

// The TPS values as the row's own program (dvbt-rx, SDRangel's toString) spells them; nullptr for a value out of range
template <size_t N> const char* name(const char* const (&names)[N], int i) { return i >= 0 && i < (int)N ? names[i] : nullptr; }
const char* const kTpsModes[] = {"2K", "8K", "4K"};           // dvbt::Mode (4K: DVB-H's)
const char* const kGuards[] = {"1/32", "1/16", "1/8", "1/4"};
const char* const kConstellations[] = {"QPSK", "16-QAM", "64-QAM"};
const char* const kRates[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
const char* const kHierarchies[] = {nullptr, "a=1", "a=2", "a=4"};   // 0: not hierarchical (the member is left out)

class DvbtSession : public PollSession {
public:
    explicit DvbtSession(double rate) : cad_(rate) {}
    bool open(const Options& o, std::string* err) {
        rate_ = o.rate;
        int narrowest = 0;
        for (int w : kDvbtWidths) {
            if (o.bandwidthMhz && w != o.bandwidthMhz) continue;
            narrowest = w;
            if (o.rate >= dvbtFloor(w)) widths_.push_back(w);
        }
        if (widths_.empty()) {
            char b[160];
            snprintf(b, sizeof b, "%.0f Hz is too low for dvbt (at least %.0f Hz)", o.rate, dvbtFloor(narrowest));
            *err = b;
            return false;
        }
        // DvbtReceiver opens no sound device. The Viterbi on the CPU, as on every computer OnAir has no GPU decoder for: the
        // same lines everywhere.
        rx_.setComputeMode(0);
        // the packets go to the demultiplexer, for the service list, and nowhere else. The receiver is not pipelined (its error
        // correction runs inside feed(), OnAir's default: a pipelined one costs the same CPU and only spreads it over a second
        // thread), so this runs on this thread and the packets of a block are read before its poll.
        rx_.setPacketCallback([this](const uint8_t* p, size_t n, double sec) {
            for (size_t i = 0; i < n; i++) ts_.feed(p + i * 188);
            ts_.advance(sec);
        });
        use(0);
        return true;
    }
    void feed(const cf32* x, size_t n) override {
        rx_.feed(x, n);
        cad_.fed(n);
        onWidth_ += n;
        // the TPS decoded (2, or 3 with the transport stream flowing): this is the width; its 2 s start again when it is lost
        if (rx_.detectLevel() >= 2) onWidth_ = 0;
        else if (widths_.size() > 1 && onWidth_ >= (uint64_t)std::llround(kDvbtTrySec * rate_)) use((at_ + 1) % widths_.size());
    }
    void poll(bool, std::vector<Json>& lines) override {
        rx_.telemetry(tel_, 0);   // published after every block: the newest is the state now
        const RxTelemetry::Dvbt& t = tel_.dvbt;
        const bool locked = t.tpsOk;
        const std::string services = locked ? serviceList() : std::string();
        auto line = [&](bool measured) {
            Json j;
            j.flag("locked", locked).str("lock", locked ? "locked" : "searching");
            if (locked) {
                j.num("bandwidth_hz", (long long)widths_[at_] * 1000000);
                auto add = [&j](const char* k, const char* v) { if (v) j.str(k, v); };
                add("mode", name(kTpsModes, t.mode));
                add("guard", name(kGuards, t.guard));
                add("constellation", name(kConstellations, t.mod));
                add("code_rate", name(kRates, t.crHp));
                if (name(kHierarchies, t.hier)) {
                    add("hierarchy", name(kHierarchies, t.hier));
                    add("lp_code_rate", name(kRates, t.crLp));
                }
                // no cell_id: OnAir's TPS decoder does not read the cell id bits (dvbt.cpp), so it is never known
                if (measured) j.num("snr_db", (double)tel_.dataSnrDb, 1);
                if (!services.empty()) j.raw("services", services);
            }
            return j;
        };
        cad_.offer(locked, line(false).done(), line(true), lines);
    }

private:
    // to width widths_[i], from its start: the receiver and the demultiplexer forget the last one
    void use(size_t i) {
        at_ = i;
        onWidth_ = 0;
        rx_.configure(rate_, widths_[i]);
        ts_.reset();
    }
    // the services as the tables have them so far, a JSON array ("" when there is none): the id, the name when the SDT gave
    // it (UTF-8: OnAir has read it from the broadcast's character table) and the kind when its type is known
    std::string serviceList() {
        const TsSnapshot s = ts_.snapshot();
        std::string a;
        for (const TsService& v : s.services) {
            Json j;
            j.num("id", (long long)v.id);
            if (!v.name.empty()) j.utf8("name", v.name);
            const std::string type = v.typeName();   // "TV", "radio", "data", "teletext", "other"; "?" while not known
            if (type != "?") j.str("kind", type == "TV" ? "tv" : type == "radio" ? "radio" : "data");
            a += (a.empty() ? "[" : ",") + j.done();
        }
        return a.empty() ? a : a + "]";
    }

    TsDemux ts_;
    DvbtReceiver rx_;
    RxTelemetry tel_;
    double rate_ = 0;
    std::vector<int> widths_;   // the widths to try, MHz
    size_t at_ = 0;             // the one in use
    uint64_t onWidth_ = 0;      // samples fed on it since it was taken or last locked
    Cadence cad_;
};

// --- video

// The standard as the row's own decoder (the node's composite.rs) spells it, from the line count (and SECAM's colour); nullptr
// while it is not known
const char* standardOf(const AtvTelemetry& t) {
    if (t.lines == 525) return "ntsc";
    if (t.lines == 625) return t.colourSystem == "SECAM" ? "secam" : "pal";
    return nullptr;
}
const char* systemOf(const char* standard) {
    const std::string s = standard;
    return s == "ntsc" ? "NTSC 525/60" : s == "secam" ? "SECAM 625/50" : "PAL 625/50";
}

class VideoSession : public PollSession {
public:
    explicit VideoSession(double rate) : cad_(rate) {}
    // setSilent and setModulation before configure: it would open a sound device on the first audio, and it applies the
    // modulation (FM video: the link is the centre of the input)
    bool open(const Options& o, std::string* err) {
        rx_.setSilent(true);
        rx_.setModulation(1);
        rx_.configure(o.rate);
        if (!rx_.ready()) {
            char b[160];
            snprintf(b, sizeof b, "%.0f Hz is too low for video (at least %.0f Hz)", o.rate, atvTuning().minSampleRate);
            *err = b;
            return false;
        }
        return true;
    }
    void feed(const cf32* x, size_t n) override {
        rx_.feed(x, n);
        cad_.fed(n);
    }
    void poll(bool, std::vector<Json>& lines) override {
        if (rx_.telemetry(tel_, seq_)) seq_ = tel_.seq;   // published every 0.25 s of signal
        const bool locked = tel_.state == 2;           // line and field sync
        const bool lineSync = tel_.state >= 1;         // line sync at least: the line rate and the SNR are measured
        const char* standard = standardOf(tel_);
        auto line = [&](bool measured) {
            Json j;
            if (standard) j.str("system", systemOf(standard)).str("standard", standard);
            j.flag("locked", locked).str("lock", locked ? "locked" : "no lock");
            if (measured && lineSync) {
                if (std::isfinite(tel_.lineHz) && tel_.lineHz > 0) j.num("line_hz", std::llround(tel_.lineHz));
                if (std::isfinite(tel_.snrDb)) j.num("snr_db", (long long)std::lround(tel_.snrDb));
            }
            return j;
        };
        cad_.offer(locked, line(false).done(), line(true), lines);
    }

private:
    AtvReceiver rx_;
    AtvTelemetry tel_;
    uint64_t seq_ = 0;
    Cadence cad_;
};

} // namespace

std::unique_ptr<PollSession> openDvbt(const Options& o, std::string* err) {
    auto s = std::make_unique<DvbtSession>(o.rate);
    if (!s->open(o, err)) return nullptr;
    return s;
}

std::unique_ptr<PollSession> openVideo(const Options& o, std::string* err) {
    auto s = std::make_unique<VideoSession>(o.rate);
    if (!s->open(o, err)) return nullptr;
    return s;
}

} // namespace onair_rx
