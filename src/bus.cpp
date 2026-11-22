// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the bus modes. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's receivers report what they decode on OnAir's automation bus (one event per message: a type and named fields) and
// hand the frames they receive to OnAir's Wireshark hub, both from inside feed(), on the thread that calls it, with no engine
// started. So one run serves every mode that has a receiver on the bus: a tap on the bus (every family) collects the events of
// a block's feed(), a sink on the hub (patches/0001) collects its packets, and after the feed() the events become JSON lines and
// the packets pcap records. A mode is a row: how to make and set up its receiver, its hub channel, and what its lines need that
// the bus lacks.
#include "bus.h"
#include "ais.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/sonde/sonde_rx.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace onair_rx {

using namespace dect2;
using Feed = std::function<void(const cf32*, size_t)>;

namespace {

// configures the receiver for the input rate; its feed when the rate suits it
template <class Rx>
bool start(std::shared_ptr<Rx> rx, const Options& o, const char* name, double minRate, Feed* feed, std::string* err) {
    rx->configure(o.rate);
    if (!rx->ready()) {
        char b[160];
        snprintf(b, sizeof b, "%.0f Hz is too low for %s (at least %.0f Hz)", o.rate, name, minRate);
        *err = b;
        return false;
    }
    *feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
    return true;
}

const automation::Value* field(const automation::Event& e, const char* k) {
    const auto it = e.f.find(k);
    return it == e.f.end() ? nullptr : &it->second;
}
std::string text(const automation::Event& e, const char* k) {
    const automation::Value* v = field(e, k);
    return v && v->t == automation::Value::Str ? v->s : std::string();
}

// --- pagers: OnAir's PagerReceiver for one protocol. Its signal offset stays 0: the channel is at 0 Hz of the input.
bool openPager(int protocols, const char* name, const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<PagerReceiver>();
    rx->setProtocols(protocols);
    return start(rx, o, name, pagerTuning().minSampleRate, feed, err);
}
bool openPocsag(const Options& o, Feed* feed, std::string* err) { return openPager(1, "pocsag", o, feed, err); }
bool openFlex(const Options& o, Feed* feed, std::string* err) { return openPager(2, "flex", o, feed, err); }

// The bus gives the speed as a name ("POCSAG 1200", "FLEX 3200/4"): its number is the baud rate, a FLEX one also has the levels
bool shapePager(const automation::Event& e, Json& j, const char* protocol) {
    if (e.type != "pager.message") return true;
    if (text(e, "protocol") != protocol) return false;   // the other protocol's page
    const std::string speed = text(e, "speed");
    const size_t sp = speed.find(' ');
    if (sp == std::string::npos) return true;
    char* end = nullptr;
    const long baud = strtol(speed.c_str() + sp + 1, &end, 10);
    if (baud > 0) j.num("baud", (long long)baud);
    if (std::string(protocol) == "FLEX" && end && *end == '/') {
        const long levels = strtol(end + 1, nullptr, 10);
        if (levels == 2 || levels == 4) j.num("levels", (long long)levels);
    }
    return true;
}
bool shapePocsag(const automation::Event& e, Json& j) { return shapePager(e, j, "POCSAG"); }
bool shapeFlex(const automation::Event& e, Json& j) { return shapePager(e, j, "FLEX"); }

// --- ACARS: OnAir's AcarsReceiver ignores a carrier at its tuned centre unless the channel is named, so the one channel is
// named: the input's centre
bool openAcars(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<AcarsReceiver>();
    rx->setCenterHz(o.freq);
    rx->setChannels({o.freq});
    return start(rx, o, "acars", acarsTuning().minSampleRate, feed, err);
}

// --- APRS: OnAir's PacketReceiver, the channel at 0 Hz of the input; its frames go to the hub's APRS channel (AX.25, link type 3)
bool openAprs(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<PacketReceiver>();
    return start(rx, o, "aprs", packetTuning().minSampleRate, feed, err);
}

// fcs_ok. What OnAir's PacketReceiver publishes (packet_rx.cpp, accept()): only frames whose AX.25 frame check sequence was right,
// as the HDLC receiver handed them over, or frames it repaired until the check fitted: "vote" (a majority over the demodulators'
// copies, on by default) and "fix bits" (one or two bits flipped, off by default). A repaired frame's summary ends with
// " [repaired by vote]", " [2 bits fixed]" or " [1 bit fixed]", and that is the only mark the bus carries. So fcs_ok is false
// exactly for those, true otherwise. (A digipeater's copy of a frame heard within a few seconds goes to the hub, not the bus.)
// path: the bus has OnAir's "WIDE1-1*,WIDE2-1" (a star marks the last digipeater that repeated it); the line has an array.
bool shapeAprs(const automation::Event& e, Json& j) {
    if (e.type != "aprs.packet") return true;
    const std::string s = text(e, "summary");
    auto endsWith = [&s](const char* t) { const std::string x = t; return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; };
    j.flag("fcs_ok", !(endsWith(" [repaired by vote]") || endsWith(" [2 bits fixed]") || endsWith(" [1 bit fixed]")));
    std::vector<std::string> path;
    const std::string p = text(e, "path");
    for (size_t i = 0; !p.empty() && i <= p.size();) {
        const size_t c = std::min(p.find(',', i), p.size());
        path.push_back(p.substr(i, c - i));
        i = c + 1;
    }
    j.arr("path", path);
    return true;
}

// --- AIS: OnAir's AisReceiver with the centre known places AIS 1 (161.975 MHz) and AIS 2 (162.025 MHz) by their own
// frequencies and decodes the ones the input holds: one of them is enough. It needs the pair's middle (162.000 MHz) 40 kHz inside
// the input's edge (ais::aisChannelsCovered), so a centre where that fails is refused rather than run deaf. The long-range
// channels 75 and 76 (message 27, for satellites) are left out. A line per message comes from the hub's sentences (ais.cpp);
// the bus's AIS events (ais.vessel, ais.anomaly, ais.behaviour, ais.cpa, ais.geofence: what OnAir makes of the messages over
// time) are all dropped, so that the mode writes only ais.message lines.
bool openAis(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<AisReceiver>();
    rx->setCenterHz(o.freq);
    rx->setChannelMask(3);
    if (!start(rx, o, "ais", aisTuning().minSampleRate, feed, err)) return false;
    if (!(ais::aisChannelsCovered(o.freq, o.rate) & 3)) {
        char b[160];
        snprintf(b, sizeof b, "ais: no AIS channel within %.0f Hz of %.0f Hz", o.rate, o.freq);
        *err = b;
        return false;
    }
    return true;
}
bool shapeAis(const automation::Event&, Json&) { return false; }
std::function<bool(const uint8_t*, size_t, Json&)> aisPackets() {
    auto joiner = std::make_shared<AisJoiner>();
    return [joiner](const uint8_t* p, size_t n, Json& j) {
        const auto sentences = joiner->add(std::string((const char*)p, n));
        return sentences && aisLine(*sentences, j);
    };
}

// --- VDL2: OnAir's Vdl2Receiver with the one channel named: the input's centre (an empty list would be the European set)
bool openVdl2(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<Vdl2Receiver>();
    rx->setCenterHz(o.freq);
    rx->setChannels({o.freq});
    return start(rx, o, "vdl2", vdl2Tuning().minSampleRate, feed, err);
}
bool flagOf(const automation::Event& e, const char* k) {
    const automation::Value* v = field(e, k);
    return v && v->t == automation::Value::Bool && v->b;
}
// icao: the frame's source address, as dumpvdl2 has it (and so ZetaSDR's VDL2 row, whose subjects are keyed on it): the
// aircraft's on a downlink, the ground station's on an uplink or a broadcast. addr_type: the source's type in dumpvdl2's
// words, "Aircraft" when from_air, else "Ground station" (the bus carries no address types; from_air is the source being an
// aircraft). dst stays as the bus has it (FFFFFF: all stations). Only vdl2.message events are lines.
bool shapeVdl2(const automation::Event& e, Json& j) {
    if (e.type != "vdl2.message") return false;
    j.str("icao", text(e, "src")).str("addr_type", flagOf(e, "from_air") ? "Aircraft" : "Ground station");
    return true;
}

// --- HFDL: OnAir's HfdlReceiver with the one channel named by its dial frequency: the input's centre. The receiver puts the
// carrier 1440 Hz above the dial frequency (kHfdlCarrierHz), so the input is centred on the dial frequency, as a USB receiver
// tuned to the listed frequency has it. Only hfdl.message events are lines; the bus's empty icao (a frame that names no
// aircraft, such as a squitter) is left out.
bool openHfdl(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<HfdlReceiver>();
    rx->setCenterHz(o.freq);
    rx->setChannels({o.freq});
    return start(rx, o, "hfdl", hfdlTuning().minSampleRate, feed, err);
}
bool shapeHfdl(const automation::Event& e, Json& j) {
    if (e.type != "hfdl.message") return false;
    if (text(e, "icao").empty()) j.omit("icao");
    return true;
}

// --- radiosondes: OnAir's SondeReceiver, which finds the carriers in the band, opens a channel on each and tells the type
// from the frame sync; one mode per type keeps only that type's sonde.update events. It runs synchronously (inside feed(),
// so that its events come on this thread and no sample is dropped when the input runs faster than real time). With --freq
// its events carry absolute frequencies (an L-band centre would lower its minimum rate to 1 MHz). An event before the sonde's
// serial is known is dropped. id is the serial spelled as rs1729's decoders write their "id" (sondeId, bus.h), serial the
// bus's own value.
bool openSonde(const Options& o, Feed* feed, std::string* err, const char* name) {
    auto rx = std::make_shared<SondeReceiver>();
    rx->setSynchronous(true);
    if (o.freq > 0) rx->setCenterMhz(o.freq * 1e-6);
    return start(rx, o, name, sondeMinSampleRateHz(o.freq * 1e-6), feed, err);
}
bool openRs41(const Options& o, Feed* feed, std::string* err) { return openSonde(o, feed, err, "rs41"); }
bool openDfm(const Options& o, Feed* feed, std::string* err) { return openSonde(o, feed, err, "dfm"); }
bool openM20(const Options& o, Feed* feed, std::string* err) { return openSonde(o, feed, err, "m20"); }
bool shapeSonde(const automation::Event& e, Json& j, const char* type) {
    if (e.type != "sonde.update" || text(e, "type") != type) return false;
    const std::string serial = text(e, "serial");
    if (serial.empty()) return false;
    j.str("id", sondeId(type, serial));
    return true;
}
bool shapeRs41(const automation::Event& e, Json& j) { return shapeSonde(e, j, "RS41"); }
bool shapeDfm(const automation::Event& e, Json& j) { return shapeSonde(e, j, "DFM"); }
bool shapeM20(const automation::Event& e, Json& j) { return shapeSonde(e, j, "M20"); }

// a bus value as a member: whole numbers without a decimal point, a non-finite number or a null left out
void member(Json& j, const std::string& k, const automation::Value& v) {
    switch (v.t) {
    case automation::Value::Num:
        if (!std::isfinite(v.n)) return;
        if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) j.num(k.c_str(), (long long)v.n);
        else { char b[32]; snprintf(b, sizeof b, "%.15g", v.n); j.raw(k.c_str(), b); }
        return;
    case automation::Value::Str: j.str(k.c_str(), v.s); return;
    case automation::Value::Bool: j.flag(k.c_str(), v.b); return;
    default: return;
    }
}

} // namespace

const BusMode kBusModes[] = {
    {"pocsag", -1, false, openPocsag, shapePocsag, nullptr, nullptr},
    {"flex", -1, false, openFlex, shapeFlex, nullptr, nullptr},
    {"acars", -1, true, openAcars, nullptr, nullptr, nullptr},
    {"aprs", wire::Aprs, false, openAprs, shapeAprs, nullptr, nullptr},
    {"ais", wire::Ais, true, openAis, shapeAis, "ais.message", aisPackets},
    {"vdl2", -1, true, openVdl2, shapeVdl2, nullptr, nullptr},
    {"hfdl", -1, true, openHfdl, shapeHfdl, nullptr, nullptr},
    {"rs41", -1, false, openRs41, shapeRs41, nullptr, nullptr},
    {"dfm", -1, false, openDfm, shapeDfm, nullptr, nullptr},
    {"m20", -1, false, openM20, shapeM20, nullptr, nullptr},
};
const size_t kBusModeCount = sizeof kBusModes / sizeof kBusModes[0];

int runBus(const BusMode& m, const Options& o, IqInput& in, Output& out) {
    Feed feed;
    std::string err;
    if (!m.open(o, &feed, &err)) { fprintf(stderr, "onair-rx: %s\n", err.c_str()); return 2; }

    // both are called on this thread, inside feed(): they only collect
    std::vector<automation::Event> events;
    automation::global().setTap([&events](const automation::Event& e) { events.push_back(e); }, automation::kAllFamilies);
    std::vector<std::vector<uint8_t>> packets;
    const auto packetLine = m.packetLines ? m.packetLines() : nullptr;
    const bool records = m.chan >= 0 && o.pcapPort;
    const bool hub = records || (m.chan >= 0 && packetLine);
    if (hub) {
        wire::Config c;   // no file, no live capture, no GSMTAP: the sink is the hub's only output
        c.dir.clear();
        c.files = c.live = c.gsmtap = false;
        wire::Hub::get().configure(c);
        wire::Hub::get().setSink([&packets, &m](wire::Chan c, int64_t, const uint8_t* p, size_t n) {
            if (c == m.chan) packets.emplace_back(p, p + n);
        });
        wire::Hub::get().setEnabled((wire::Chan)m.chan, true);
    }

    RecordClock clock;
    uint64_t lines = 0, sent = 0;
    std::vector<cf32> iq(1 << 14);
    for (size_t n; (n = in.read(iq)) > 0;) {
        const uint64_t first = in.samples() - n;
        feed(iq.data(), n);
        for (const automation::Event& e : events) {
            Json j;
            j.str("event", e.type).num("t_sample", (long long)first);
            if (m.shape && !m.shape(e, j)) continue;
            for (const auto& [k, v] : e.f) if (!j.has(k)) member(j, k, v);   // what shape wrote stands
            out.line(j.done());
            lines++;
        }
        events.clear();
        if (packetLine)
            for (const auto& p : packets) {
                Json j;
                j.str("event", m.packetEvent).num("t_sample", (long long)first);
                if (!packetLine(p.data(), p.size(), j)) continue;
                out.line(j.done());
                lines++;
            }
        if (records && !packets.empty()) {
            // at the block's first sample. A packet the channel has as application data is wrapped as OnAir's capture files
            // have it (wire.cpp): in IPv4 + UDP to the channel's port, so that the port's dissector reads it
            const wire::ChanInfo& ci = wire::kChans[m.chan];
            const int64_t t = clock.at(in, (double)first / o.rate);
            for (const auto& p : packets) {
                if (ci.encap == wire::Encap::Direct) { out.packet(t, p.data(), p.size()); continue; }
                const wire::Bytes w = wire::udpInIpv4(ci.udpPort, p.data(), p.size());
                out.packet(t, w.data(), w.size());
            }
            sent += packets.size();
        }
        packets.clear();
    }
    automation::global().setTap({}, 0);
    if (hub) {
        wire::Hub::get().setSink({});
        wire::Hub::get().shutdown();
    }
    fprintf(stderr, "onair-rx: %s: %llu lines", m.name, (unsigned long long)lines);
    if (records) fprintf(stderr, ", %llu packets", (unsigned long long)sent);
    fprintf(stderr, "\n");
    return 0;
}

} // namespace onair_rx
