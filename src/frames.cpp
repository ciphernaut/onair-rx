// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: lines made from Wireshark hub packets (zigbee, thread, zwave, wifi). Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// Copyright (C) 2026 the onair-rx authors
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// It is distributed WITHOUT ANY WARRANTY; see the LICENSE file.
#include "frames.h"
#include "dect2/thread/thread_proto.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zwave/zwave_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <strings.h>

namespace onair_rx {

using namespace dect2;

namespace {

std::string hex(uint64_t v, int digits) {
    char b[24];
    snprintf(b, sizeof b, "%0*llx", digits, (unsigned long long)v);
    return b;
}

// --- Zigbee and Thread: the PSDU, as an 802.15.4 frame
// frame_type: the MAC frame's type in the lines' lower case ("beacon", "data", "ack", "command"; OnAir's other names for the
// 2015 types, "multipurpose", "fragment", "extended"); a reserved type has none. src, dst: the short address (4 hex digits) or
// the extended one (16), as the frame carries it; none when it has none (an ack). pan: the destination's PAN, else the source's.
bool macMembers(const zigbee::Frame& f, Json& j) {
    if (!f.ok) return false;
    std::string type = zigbee::frameTypeName(f.type);
    if (type == "Reserved") return false;
    if (type == "MAC cmd") type = "command";
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return (char)tolower(c); });
    j.str("frame_type", type);
    if (f.seq >= 0) j.num("seq", (long long)f.seq);
    if (f.hasDestPan) j.str("pan", hex(f.destPan, 4));
    else if (f.hasSrcPan) j.str("pan", hex(f.srcPan, 4));
    if (f.srcMode == 2) j.str("src", hex(f.srcShort, 4));
    else if (f.srcMode == 3) j.str("src", hex(f.srcExt, 16));
    if (f.destMode == 2) j.str("dst", hex(f.destShort, 4));
    else if (f.destMode == 3) j.str("dst", hex(f.destExt, 16));
    return true;
}

} // namespace

// zigbee.frame: the MAC members, the network addresses when the frame's NWK header is readable (a NWK payload that is encrypted
// still has its header in the clear), secured when any layer is
std::function<bool(const uint8_t*, size_t, Json&)> zigbeePackets() {
    return [](const uint8_t* p, size_t n, Json& j) {
        const zigbee::Frame f = zigbee::parse(p, n);
        if (!macMembers(f, j)) return false;
        if (f.hasNwk) j.str("nwk_src", hex(f.nwkSrc, 4)).str("nwk_dst", hex(f.nwkDest, 4));
        j.flag("secured", f.secured || f.nwkSecured || f.apsSecured).num("len", (long long)n);
        return true;
    };
}

// thread.frame: the MAC members, secured when the link layer is, and kind, OnAir's own line for what the frame is (thread::analyse:
// "MLE Discovery Request", "Thread beacon", "6LoWPAN ..."); Thread has no Zigbee network layer
std::function<bool(const uint8_t*, size_t, Json&)> threadPackets() {
    return [](const uint8_t* p, size_t n, Json& j) {
        zigbee::Frame f = zigbee::parse(p, n);
        if (!macMembers(f, j)) return false;
        const thread::Frame t = thread::analyse(f, p, n);
        j.flag("secured", f.secured || t.encrypted).num("len", (long long)n);
        const std::string kind = t.info.empty() ? f.info : t.info;
        if (!kind.empty()) j.utf8("kind", kind);
        return true;
    };
}

// --- Z-Wave: the data rate (0 to 2) and the MPDU
namespace {
struct ZwaveWhereState {
    double centreHz = 0, rate = 0;
    std::string region;
} g_zwave;

// the channels of the region the input covers: the receiver's own test (zwave_rx.cpp, rebuild)
bool zwCovered(double mhz, double centreHz, double rate) { return rate > 0 && std::fabs(mhz * 1e6 - centreHz) < 0.45 * rate; }
}

void zwaveWhere(double centreHz, double rate, const std::string& region) {
    g_zwave.centreHz = centreHz;
    g_zwave.rate = rate;
    g_zwave.region = region;
}

bool zwaveRegion(double centreHz, double rate, const std::string& name, std::string* id, std::string* err) {
    char band[64];
    snprintf(band, sizeof band, "%.3f-%.3f MHz", (centreHz - 0.45 * rate) * 1e-6, (centreHz + 0.45 * rate) * 1e-6);
    auto inside = [&](const zwave::Region& r) {
        int n = 0;
        for (const zwave::Chan& c : r.chans) n += zwCovered(c.mhz, centreHz, rate);
        return n;
    };
    const zwave::Region* pick = nullptr;
    if (!name.empty()) {
        std::string names;
        for (const zwave::Region& r : zwave::regions()) {
            if (strcasecmp(name.c_str(), r.id) == 0) pick = &r;
            names += std::string(names.empty() ? "" : ", ") + r.id;
        }
        if (!pick) { *err = "zwave: no region " + name + " (" + names + ")"; return false; }
        if (!inside(*pick)) { *err = std::string("zwave: region ") + pick->id + " has no channel within " + band; return false; }
    } else {
        int best = 0;
        for (const zwave::Region& r : zwave::regions())
            if (const int n = inside(r); n > best) { best = n; pick = &r; }
        if (!pick) { *err = std::string("zwave: no region has a channel within ") + band; return false; }
    }
    *id = pick->id;
    return true;
}

// zwave.frame: home_id, src and dst (node ids; a multicast has no dst), frame_type (zwave::headerName, "routed" for a routed
// frame), rate_bps, freq_hz, secured (an S0 or S2 payload, which stays encrypted) and len (the MPDU's, its check included)
std::function<bool(const uint8_t*, size_t, Json&)> zwavePackets() {
    // freq_hz by data rate, from the region and the input: the channel when the region has only one of that rate inside the input
    long long freq[3] = {0, 0, 0};
    if (const zwave::Region* r = zwave::region(g_zwave.region))
        for (int rate = 1; rate <= 3; rate++) {
            long long only = 0;
            bool many = false;
            for (const zwave::Chan& c : r->chans) {
                if (c.rate != rate || !zwCovered(c.mhz, g_zwave.centreHz, g_zwave.rate)) continue;
                const long long hz = std::llround(c.mhz * 1e6);
                if (only && only != hz) many = true;
                only = hz;
            }
            if (!many) freq[rate - 1] = only;
        }
    return [freq](const uint8_t* p, size_t n, Json& j) {
        if (n < 2 || p[0] > 2) return false;
        const int rate = p[0] + 1;
        zwave::Frame f;
        if (!zwave::parse(p + 1, n - 1, rate, f) || !f.checkOk) return false;
        const std::string home = zwave::homeText(f.homeId);
        std::string lower;
        for (const char c : home) lower += (char)tolower((unsigned char)c);
        j.str("home_id", lower).num("src", (long long)f.src);
        if (f.headerType != zwave::kHdrMulticast) j.num("dst", (long long)f.dst);
        j.str("frame_type", f.routed ? "routed" : zwave::headerName(f.headerType)).num("rate_bps", (long long)zwave::bitRate(rate));
        if (freq[rate - 1]) j.num("freq_hz", freq[rate - 1]);
        j.flag("secured", zwave::encryption(f.payload) != 0).num("len", (long long)f.len);
        return true;
    };
}

// --- Wi-Fi: a radiotap header (OnAir's: the flags, the channel when the centre is known, the signal level) and the 802.11 frame,
// the FCS taken off. The line is a beacon, a probe response or a FILS discovery frame: the frames OnAir's parser reads an access
// point from (wifi::parseFrame); another frame is a record only.
namespace {
struct Radiotap {
    size_t len = 0;
    int freqMhz = 0;   // the channel field's frequency, 0 when the header has none
};
// the header's length and its channel field's frequency (OnAir's header has the flags, the channel when the centre is known and
// the signal level, and no data rate). The fields are placed from the present flags: TSFT, flags, rate, channel, FHSS, antenna signal
bool radiotap(const uint8_t* p, size_t n, Radiotap& r) {
    if (n < 8 || p[0] != 0) return false;
    r.len = (size_t)(p[2] | p[3] << 8);
    if (r.len < 8 || r.len > n) return false;
    const uint32_t present = (uint32_t)(p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24);
    size_t at = 8;
    if (present & 1u << 31) return true;   // extended present words: the fields are left alone
    static const struct { int bit; size_t size, align; } fields[] = {{0, 8, 8}, {1, 1, 1}, {2, 1, 1}, {3, 4, 2}, {4, 2, 2}, {5, 1, 1}};
    for (const auto& f : fields) {
        if (!(present & 1u << f.bit)) continue;
        at = (at + f.align - 1) / f.align * f.align;
        if (at + f.size > r.len) return true;
        if (f.bit == 3) r.freqMhz = p[at] | p[at + 1] << 8;
        at += f.size;
    }
    return true;
}
}

std::function<bool(const uint8_t*, size_t, Json&)> wifiPackets() {
    return [](const uint8_t* p, size_t n, Json& j) {
        Radiotap rt;
        if (!radiotap(p, n, rt)) return false;
        const int band = rt.freqMhz > 0 ? wifi::bandOfMhz(rt.freqMhz) : -1;
        wifi::Frame f;
        if (!wifi::parseFrame(p + rt.len, n - rt.len, f, band) || !(f.beacon || f.probeResp || f.fils)) return false;
        std::string bssid;
        for (const uint8_t b : f.bssid) bssid += hex(b, 2);
        j.str("frame_type", f.fils ? "fils_discovery" : f.probeResp ? "probe_response" : "beacon").str("bssid", bssid);
        if (!f.hidden && !f.ssid.empty()) j.utf8("ssid", f.ssid);
        const int channel = f.channel > 0 ? f.channel : band >= 0 ? wifi::bandChannelOf(band, rt.freqMhz) : 0;
        if (channel > 0) j.num("channel", (long long)channel);
        if (rt.freqMhz > 0) j.num("freq_hz", (long long)rt.freqMhz * 1000000);
        if (!f.fils) j.str("security", wifi::securityName(f.security, f.enterprise));   // a FILS discovery frame says none
        j.num("len", (long long)(n - rt.len));
        return true;
    };
}

} // namespace onair_rx
