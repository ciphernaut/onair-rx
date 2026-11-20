// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the adsb mode. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's AdsbReceiver does all the decoding. Its frame callback hands over every good Mode S frame; the frames are taken
// after each feed() (so the receiver's CPU guard times only its own work) and, one at a time in order, applied to a second
// OnAir AdsbTracker. That table is the receiver's own aircraft table rebuilt from the same frames, and its snapshot gives the
// aircraft's state just after this frame (the receiver's telemetry() only refreshes every 0.25 s of signal). Each frame
// becomes one JSON line and one pcap record with the same bytes.
#include "rx.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/adsb/adsb_track.h"
#include "dect2/net/feeds.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace onair_rx {

using namespace dect2;

namespace {

// The SBS (BaseStation) message type of a frame, 1 to 8, as OnAir's SBS writer classifies it; 0 when it has none
int sbsType(const AdsbFrame& f) {
    FeedAdsbFrame ff;
    memcpy(ff.bytes, f.bytes, sizeof ff.bytes);
    ff.bits = f.bits;
    ff.icao = f.icao;
    ff.msg = f.msg;
    const std::string l = feedfmt::sbsLine(ff, 0);
    return l.rfind("MSG,", 0) == 0 ? atoi(l.c_str() + 4) : 0;
}

// Whether a frame says if the aircraft is on the ground: the cases in which OnAir's table (AdsbTracker::applyMsg) sets it
bool statesGround(const AdsbFrame& f) {
    const adsb::Msg& m = f.msg;
    if (m.tc >= 5 && m.tc <= 8) return true;
    if (m.tc >= 9 && m.tc <= 22 && m.tc != 19) return true;
    if (f.df == 4 || f.df == 5 || f.df == 20 || f.df == 21) return m.fs >= 0 && m.fs <= 3;
    if (f.df == 0 || f.df == 16) return true;
    if (f.df == 11 || f.df == 17) return m.ca == 4 || m.ca == 5;
    return false;
}

std::string trimCallsign(std::string s) {   // as OnAir's SBS writer trims it
    while (!s.empty() && (s.back() == ' ' || s.back() == '_' || s.back() == '@')) s.pop_back();
    return s;
}

} // namespace

int runAdsb(const Options& o, IqInput& in, Output& out) {
    AdsbReceiver rx;
    std::vector<AdsbFrame> pending;
    rx.setFrameCallback([&pending](const AdsbFrame& f) { pending.push_back(f); });   // called from feed(), on this thread
    rx.configure(o.rate);
    if (!rx.ready()) {
        fprintf(stderr, "onair-rx: %.0f Hz is too low for adsb (at least %.0f Hz)\n", o.rate, adsbTuning().minSampleRate);
        return 2;
    }

    AdsbTracker table;
    AdsbTelemetry snap;
    double nextTick = 0.25;                              // the receiver's report interval: expiry runs on the same clock
    std::unordered_map<uint32_t, double> groundStated;   // table key -> signal time of its last frame that stated air or ground
    int64_t anchorUsec = 0;                              // wall clock of signal time 0
    uint64_t frames = 0, lines = 0;

    auto emit = [&](const AdsbFrame& f) {
        frames++;
        while (f.timeSec >= nextTick) {
            table.tick(nextTick);
            for (auto it = groundStated.begin(); it != groundStated.end();)   // long gone from the table
                it = nextTick - it->second > 600 ? groundStated.erase(it) : std::next(it);
            nextTick += 0.25;
        }
        table.apply(f);
        const bool nonIcao = f.df == 18 && (f.msg.ca == 1 || f.msg.ca == 5);   // the table's key (AdsbTracker::apply)
        const uint32_t key = f.icao | (nonIcao ? 0x1000000u : 0u);
        if (f.newAircraft) groundStated.erase(key);
        if (statesGround(f)) groundStated[key] = f.timeSec;
        table.snapshot(snap, f.timeSec);
        const AdsbAircraft* a = nullptr;
        for (const AdsbAircraft& x : snap.aircraft) if (x.icao == key) { a = &x; break; }

        // the record: signal time on the wall clock of the first sample, moved on when the input fell behind real time
        const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!anchorUsec) anchorUsec = in.startUsec();
        int64_t t = anchorUsec + (int64_t)std::llround(f.timeSec * 1e6);
        if (t < now - 1000000) { anchorUsec += now - t; t = now; }
        out.packet(t, f.bytes, (size_t)f.bits / 8);

        char hex[8];
        snprintf(hex, sizeof hex, "%06X", f.icao & 0xFFFFFF);
        Json j;
        j.str("icao", hex).num("df", (long long)f.df);
        if (f.msg.tc >= 0) j.num("tc", (long long)f.msg.tc);
        j.num("level_dbfs", (double)f.levelDbfs, 1);
        if (const int m = sbsType(f)) j.num("msg", (long long)m);
        if (a) {   // the aircraft as the table has it now (it is always there, as the most recently heard)
            const std::string call = trimCallsign(a->callsign);
            if (!call.empty()) j.str("callsign", call);
            if (a->hasAlt) j.num("altitude_ft", (long long)a->altFt);
            if (a->hasPos) j.num("lat", a->lat, 6).num("lon", a->lon, 6);
            if (a->hasSpeed && a->speedKind == 0) j.num("ground_speed_kt", (double)a->speedKt, 1);
            if (a->hasHeading && a->headingIsTrack) j.num("track_deg", (double)a->headingDeg, 1);
            if (a->hasSquawk) { char sq[8]; snprintf(sq, sizeof sq, "%04d", a->squawk % 10000); j.str("squawk", sq); }
            if (groundStated.count(key)) j.flag("on_ground", a->ground);
        }
        out.line(j.done());
        lines++;
    };

    std::vector<cf32> iq(1 << 14);
    for (size_t n; (n = in.read(iq)) > 0;) {
        rx.feed(iq.data(), n);
        for (const AdsbFrame& f : pending) emit(f);
        pending.clear();
    }
    fprintf(stderr, "onair-rx: adsb: %llu frames, %llu lines\n", (unsigned long long)frames, (unsigned long long)lines);
    return 0;
}

} // namespace onair_rx
