// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the adsb mode. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's AdsbReceiver does all the decoding. Its frame callback hands over every good Mode S frame (checked, repaired,
// and decoded by OnAir's decodeMsg into AdsbFrame::msg); the frames are taken after each feed(), so the receiver's CPU
// guard times only its own work. Each frame becomes one JSON line and one pcap record with the same bytes.
//
// A line carries only what its own frame established, as an SBS (BaseStation) line does: the members OnAir's SBS writer
// (feedfmt::sbsLine) fills for the frame's message type, from the frame's decoded message. A position is the exception: it
// takes the aircraft's earlier frames (a CPR pair, or a reference fix), so it comes from OnAir's AdsbTracker, and a line has
// one only when its frame committed a fix (what the tracker passes its own feeds as FeedAdsbFrame::hasPos). OnAir hands that
// flag to sockets only, so each aircraft gets an AdsbTracker of its own, fed every frame of that address; its one-aircraft
// snapshot, taken only on frames with a CPR position, says whether this frame set the fix (posAgeSec 0). Position decoding
// in the tracker reads only the aircraft's own record (no reference position is set), so this is the receiver's own result.
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

// The SBS message type of a frame, 1 to 8, as OnAir's SBS writer (feedfmt::sbsLine) classifies it; 0 when it has none
int sbsType(const AdsbFrame& f) {
    FeedAdsbFrame ff;
    memcpy(ff.bytes, f.bytes, sizeof ff.bytes);
    ff.bits = f.bits;
    ff.icao = f.icao;
    ff.msg = f.msg;
    const std::string l = feedfmt::sbsLine(ff, 0);
    return l.rfind("MSG,", 0) == 0 ? atoi(l.c_str() + 4) : 0;
}

// What a frame says about air or ground: 1 on the ground, 0 airborne, -1 nothing. Mirrors AdsbTracker::applyMsg's
// "on the ground or in the air" chain (adsb_track.cpp), the only place OnAir decides it; adsb::Msg::ground has no "unknown".
int groundOf(const AdsbFrame& f) {
    const adsb::Msg& m = f.msg;
    if (m.tc >= 5 && m.tc <= 8) return 1;
    if (m.tc >= 9 && m.tc <= 22 && m.tc != 19) return 0;
    if (f.df == 4 || f.df == 5 || f.df == 20 || f.df == 21) return m.fs == 0 || m.fs == 2 ? 0 : m.fs == 1 || m.fs == 3 ? 1 : -1;
    if (f.df == 0 || f.df == 16) return m.vs == 1 ? 1 : 0;
    if (f.df == 11 || f.df == 17) return m.ca == 4 ? 1 : m.ca == 5 ? 0 : -1;
    return -1;
}

// Mirrors trimRight in OnAir's feeds_fmt.cpp, which sbsLine applies to the callsign
std::string trimCallsign(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '_' || s.back() == '@')) s.pop_back();
    return s;
}

struct Aircraft {
    AdsbTracker table;   // this address only
    double lastSec = 0;
};

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

    std::unordered_map<uint32_t, Aircraft> aircraft;   // by the tracker's key
    AdsbTelemetry snap;
    double nextPrune = 10;
    int64_t anchorUsec = 0;                             // wall clock of signal time 0
    uint64_t frames = 0;

    auto emit = [&](const AdsbFrame& f) {
        frames++;
        const adsb::Msg& m = f.msg;
        const int type = sbsType(f);

        // the position, when this frame committed one
        const bool nonIcao = f.df == 18 && (m.ca == 1 || m.ca == 5);   // the tracker's key (AdsbTracker::apply)
        const uint32_t key = f.icao | (nonIcao ? 0x1000000u : 0u);
        Aircraft& ac = aircraft[key];
        ac.table.tick(f.timeSec);   // OnAir's expiry: a record silent too long starts again
        ac.table.apply(f);
        ac.lastSec = f.timeSec;
        bool hasPos = false;
        double lat = 0, lon = 0;
        if ((type == 2 || type == 3) && m.hasCpr) {
            ac.table.snapshot(snap, f.timeSec);
            if (!snap.aircraft.empty() && snap.aircraft[0].hasPos && snap.aircraft[0].posAgeSec == 0.f) {
                hasPos = true;
                lat = snap.aircraft[0].lat;
                lon = snap.aircraft[0].lon;
            }
        }
        if (f.timeSec >= nextPrune) {   // trackers of aircraft long gone (OnAir drops a record after 60 s by default)
            for (auto it = aircraft.begin(); it != aircraft.end();) it = f.timeSec - it->second.lastSec > 300 ? aircraft.erase(it) : std::next(it);
            nextPrune = f.timeSec + 10;
        }

        // the record: signal time on the wall clock of the first sample, moved on when the input falls behind real time
        const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!anchorUsec) anchorUsec = in.startUsec();
        int64_t t = anchorUsec + (int64_t)std::llround(f.timeSec * 1e6);
        if (t < now - 100000) { anchorUsec += now - t; t = now; }
        out.packet(t, f.bytes, (size_t)f.bits / 8);

        // the line: the members sbsLine fills for this message type, from this frame
        char hex[8];
        snprintf(hex, sizeof hex, "%06X", f.icao & 0xFFFFFF);
        Json j;
        j.str("icao", hex).num("df", (long long)f.df);
        if (m.tc >= 0) j.num("tc", (long long)m.tc);
        j.num("level_dbfs", (double)f.levelDbfs, 1);
        if (type) j.num("msg", (long long)type);
        if (type == 1 && m.hasIdent) {
            const std::string call = trimCallsign(m.callsign);
            if (!call.empty()) j.str("callsign", call);
        }
        if (m.hasAlt && type != 0 && type != 1 && type != 2 && type != 4) j.num("altitude_ft", (long long)m.altFt);
        if (hasPos) j.num("lat", lat, 6).num("lon", lon, 6);
        if (type == 2) {
            if (m.hasMove) j.num("ground_speed_kt", m.moveKt, 1);
            if (m.hasTrack) j.num("track_deg", m.trackDeg, 1);
        }
        if (type == 4) {
            if (m.hasVel && m.speedKind == 0) j.num("ground_speed_kt", m.speedKt, 1);
            if (m.hasHeading && m.headingIsTrack) j.num("track_deg", m.headingDeg, 1);
            if (m.hasVrate) j.num("vertical_rate_fpm", (long long)m.vrateFpm);
        }
        if (type == 6 && m.hasSquawk) { char sq[8]; snprintf(sq, sizeof sq, "%04d", m.squawk % 10000); j.str("squawk", sq); }
        if (const int g = groundOf(f); g >= 0) j.flag("on_ground", g == 1);
        out.line(j.done());
    };

    std::vector<cf32> iq(1 << 14);
    for (size_t n; (n = in.read(iq)) > 0;) {
        rx.feed(iq.data(), n);
        for (const AdsbFrame& f : pending) emit(f);
        pending.clear();
    }
    fprintf(stderr, "onair-rx: adsb: %llu frames\n", (unsigned long long)frames);
    return 0;
}

} // namespace onair_rx
