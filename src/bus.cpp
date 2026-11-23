// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the bus modes. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's receivers report what they decode on OnAir's automation bus (one event per message: a type and named fields) and
// hand the frames they receive to OnAir's Wireshark hub, both from inside feed(), on the thread that calls it, with no engine
// started. So one run serves every mode that has a receiver on the bus: a tap on the bus (every family) collects the events of
// a block's feed(), a tap on the hub (HubTap, hub_tap.h, locked: a receiver may publish from a thread of its own) collects its
// packets, and after the feed() the events become JSON lines and the packets pcap records. (Wi-SUN demodulates on a thread of
// its own, but hands its frames to the hub on the feeding thread, inside feed() or flush(); it puts nothing on the bus.) A mode
// is a row: its event (the only bus event it writes), how to make and set up its receiver, its hub channel, and what its lines
// need that the bus lacks.
#include "bus.h"
#include "ais.h"
#include "frames.h"
#include "hub_tap.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/lorawan/lora_bank.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/sonde/sonde_rx.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <strings.h>

namespace onair_rx {

using namespace dect2;

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
// lat, lon: left out for an object or an item (the position is the object's, not the sender's) and for a third-party packet
// (info "}...": the position is the station's inside it), as OnAir keeps them off the sender (packet_rx.cpp, accept()).
// A message (info ":...") is its aprs.packet line only: the bus's aprs.message for it is not the row's event.
bool shapeAprs(const automation::Event& e, Json& j) {
    const std::string type = text(e, "type");
    if (type == "Object" || type == "Item" || text(e, "info").rfind("}", 0) == 0) j.omit("lat").omit("lon");
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
// time) are not the row's event, and a bus event named ais.message would be a second line of a message: all are dropped, so
// that the mode writes only the hub's ais.message lines.
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
PacketLine aisPackets() {
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
// aircraft). dst stays as the bus has it (FFFFFF: all stations).
bool shapeVdl2(const automation::Event& e, Json& j) {
    j.str("icao", text(e, "src")).str("addr_type", flagOf(e, "from_air") ? "Aircraft" : "Ground station");
    return true;
}

// --- HFDL: OnAir's HfdlReceiver with the one channel named by its dial frequency: the input's centre. The receiver puts the
// carrier 1440 Hz above the dial frequency (kHfdlCarrierHz), so the input is centred on the dial frequency, as a USB receiver
// tuned to the listed frequency has it. The bus's empty icao (a frame that names no aircraft, such as a squitter) is left out.
bool openHfdl(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<HfdlReceiver>();
    rx->setCenterHz(o.freq);
    rx->setChannels({o.freq});
    return start(rx, o, "hfdl", hfdlTuning().minSampleRate, feed, err);
}
bool shapeHfdl(const automation::Event& e, Json& j) {
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
    if (text(e, "type") != type) return false;
    const std::string serial = text(e, "serial");
    if (serial.empty()) return false;
    j.str("id", sondeId(type, serial));
    return true;
}
bool shapeRs41(const automation::Event& e, Json& j) { return shapeSonde(e, j, "RS41"); }
bool shapeDfm(const automation::Event& e, Json& j) { return shapeSonde(e, j, "DFM"); }
bool shapeM20(const automation::Event& e, Json& j) { return shapeSonde(e, j, "M20"); }

// an address as the lines spell one: lower-case hex digits without separators ("C0:FF:EE:12:34:56" is "c0ffee123456")
std::string address(const std::string& s) {
    std::string a;
    for (const char c : s)
        if (isxdigit((unsigned char)c)) a += (char)tolower((unsigned char)c);
    return a;
}

// --- Bluetooth LE: OnAir's BleReceiver, the advertising channel at 0 Hz of the input. It dewhitens a packet for each of the three
// advertising channels in turn and keeps the one whose CRC passes, so it needs no channel; --freq, when given, only says which
// it tries first. ble.device comes per good packet with an address (the advertiser's; the scanner's for a scan or connection
// request): mac in the lines' spelling, name left out when the packet had none
bool openBle(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<BleReceiver>();
    if (o.freq > 0) rx->setCenterHz(o.freq);
    return start(rx, o, "ble", bleTuning().minSampleRate, feed, err);
}
bool shapeBle(const automation::Event& e, Json& j) {
    const std::string mac = address(text(e, "mac"));
    if (mac.size() != 12) return false;
    j.str("mac", mac);
    if (text(e, "name").empty()) j.omit("name");
    return true;
}

// --- LoRaWAN: OnAir's LorawanReceiver on every channel of one regional plan inside the input, at the spreading factors the
// plan's data rates use, normal and inverted IQ, and the plan's FSK channel. The receiver does not choose a plan: --plan names
// it, or it is the plan of OnAir's table with the most channels inside the input around --freq (the receiver's own test:
// lorabank::Bank::fits), a tie going to the table's order. No plan with a channel inside the input, or a named plan without
// one, is refused. freq_hz is the channel's centre, from the bus event's frequency; devaddr in the lines' spelling
int lorawanChannelsInside(const Options& o, int plan) {   // the channels the receiver searches with OnAir's default settings
    int n = 0;
    for (const lorawan::PlanChannel& c : lorawan::selectChannels(plan, 0, true)) {
        lorabank::Channel probe;
        probe.offsetHz = c.mhz * 1e6 - o.freq;
        probe.bwHz = c.bwKhz * 1e3;
        if (lorabank::Bank::fits(o.rate, probe)) n++;
    }
    return n;
}
bool openLorawan(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<LorawanReceiver>();
    if (!start(rx, o, "lorawan", lorawanTuning().minSampleRate, feed, err)) return false;
    char band[64];
    snprintf(band, sizeof band, "%.3f-%.3f MHz", (o.freq - o.rate / 2) * 1e-6, (o.freq + o.rate / 2) * 1e-6);
    int plan = -1;
    if (!o.plan.empty()) {
        std::string names;
        for (int p = 0; p < lorawan::kPlans; p++) {
            if (strcasecmp(o.plan.c_str(), lorawan::planName(p)) == 0) plan = p;
            names += std::string(p ? ", " : "") + lorawan::planName(p);
        }
        if (plan < 0) { *err = "lorawan: no plan " + o.plan + " (" + names + ")"; return false; }
        if (!lorawanChannelsInside(o, plan)) {
            *err = std::string("lorawan: plan ") + lorawan::planName(plan) + " has no channel within " + band;
            return false;
        }
    } else {
        for (int p = 0, best = 0; p < lorawan::kPlans; p++)
            if (const int n = lorawanChannelsInside(o, p); n > best) { best = n; plan = p; }
        if (plan < 0) { *err = std::string("lorawan: no channel plan has a channel within ") + band; return false; }
    }
    LorawanSettings s;   // OnAir's defaults otherwise: every sub-band, the downlinks, at most 24 channels (the nearest the centre)
    s.plan = plan;
    rx->setSettings(s);
    rx->setCenterHz(o.freq);
    return true;
}
bool shapeLorawan(const automation::Event& e, Json& j) {
    if (std::isfinite(e.freqHz) && e.freqHz > 0) j.num("freq_hz", (long long)std::llround(e.freqHz));
    const std::string a = address(text(e, "devaddr"));
    if (!a.empty()) j.str("devaddr", a);
    return true;
}

// --- Zigbee, Thread, Z-Wave and Wi-Fi: OnAir's receivers put nothing on the bus; their lines come from the hub's packets (frames.h).
// All four place their channel by --freq.
// Zigbee and Thread: one 802.15.4 receiver (Thread: setThreadMode). --freq picks the PHY, O-QPSK at 2.4 GHz, the sub-GHz BPSK
// channels below 1 GHz, and which channel the frames are noted on; the channel is at 0 Hz of the input.
bool openZigbee(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<ZigbeeReceiver>();
    rx->setCenterHz(o.freq);
    return start(rx, o, "zigbee", zigbeeTuning().minSampleRate, feed, err);
}
bool openThread(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<ZigbeeReceiver>();
    rx->setThreadMode(true);
    rx->setCenterHz(o.freq);
    return start(rx, o, "thread", threadTuning().minSampleRate, feed, err);
}
// Z-Wave: every channel of the region inside 0.45 of the rate around --freq is listened to. --region names the region; without it
// the receiver takes the one with most channels inside the input. A region with no channel inside the input is refused.
bool openZwave(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<ZwaveReceiver>();
    if (!start(rx, o, "zwave", zwaveTuning().minSampleRate, feed, err)) return false;
    std::string region;
    if (!zwaveRegion(o.freq, o.rate, o.region, &region, err)) return false;
    if (!o.region.empty()) rx->setRegion(region);
    rx->setCenterHz(o.freq);
    return true;
}
// Wi-Fi: the band is the --freq's (2.4 GHz: the DSSS frames of the channel at 0 Hz; 5 and 6 GHz: the 20 MHz channels inside a
// wide input)
bool openWifi(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<WifiReceiver>();
    rx->setCenterHz(o.freq);
    return start(rx, o, "wifi", wifiTuning().minSampleRate, feed, err);
}

// Wi-SUN: OnAir's WisunReceiver listens on every channel of the band plan inside 0.45 of the rate around --freq, at the bit
// rates its channel plans carry there (50, 100 and 150 kbit/s). --region names the band plan (OnAir's ids, case does not matter);
// without it the receiver picks the plan by --freq. A plan with no channel inside the input is refused (the receiver would never
// hear a frame): open asks the receiver itself, from its first report.
// It demodulates the bursts on a thread of its own, and hands each frame over (to the hub and its frame log) at the start of a
// later feed() or in a report. So that a frame's line comes at the same block whatever the threads did, each block is followed by
// flush(): the decoder has finished every burst that ended in it, and its frames are handed over before the block is drained.
// The receiver then waits for the decoder instead of dropping bursts (setOffline). With --drop it does neither, as it runs live:
// bursts that come while the decoder is busy are dropped, a frame's line comes with the block in which it was handed over, and the
// report a line takes its channel and bit rate from may not have it yet. At the end of input the flush hands over the last frames
// either way. It waits only for the bursts already queued (a burst the input cuts off is never queued), at most kQueueMax
// (wisun_rx.cpp) of them with --drop, one block's without.
struct WisunFeed {
    std::shared_ptr<WisunReceiver> rx;
    bool eachBlock;   // flush after each block (no --drop)
    void operator()(const cf32* x, size_t n) const {
        if (x) rx->feed(x, n);
        if (eachBlock || !x) rx->flush();
    }
};
bool openWisun(const Options& o, Feed* feed, std::string* err) {
    auto rx = std::make_shared<WisunReceiver>();
    if (!start(rx, o, "wisun", wisunTuning().minSampleRate, feed, err)) return false;
    std::string region;
    if (!o.region.empty()) {
        std::string names;
        for (const wisun::BandPlan& b : wisun::bandPlans()) {
            if (strcasecmp(o.region.c_str(), b.id) == 0) region = b.id;
            names += std::string(names.empty() ? "" : ", ") + b.id;
        }
        if (region.empty()) { *err = "wisun: no region " + o.region + " (" + names + ")"; return false; }
        rx->setRegion(region);
    }
    rx->setCenterHz(o.freq);
    rx->setDialHz(o.freq);
    rx->setOffline(!o.drop);
    // the receiver sets itself up on its first feed; its first report says the plan it took and the channels inside the input
    rx->feed(nullptr, 0);
    rx->flush();
    WisunTelemetry t;
    rx->telemetry(t, 0);
    if (t.channelsCovered == 0) {
        char b[160];
        if (!region.empty())
            snprintf(b, sizeof b, "wisun: region %s has no channel within %.3f-%.3f MHz", t.region.c_str(),
                     (o.freq - 0.45 * o.rate) * 1e-6, (o.freq + 0.45 * o.rate) * 1e-6);
        else
            snprintf(b, sizeof b, "wisun: region %s (OnAir's pick for %.3f MHz) has no channel within %.3f-%.3f MHz", t.region.c_str(),
                     o.freq * 1e-6, (o.freq - 0.45 * o.rate) * 1e-6, (o.freq + 0.45 * o.rate) * 1e-6);
        *err = b;
        return false;
    }
    *feed = WisunFeed{rx, !o.drop};
    return true;
}
// its lines (frames.h) read the frame log of the receiver open made
PacketLine wisunLines(const Options&, const Feed& feed) {
    const WisunFeed* f = feed.target<WisunFeed>();
    return f ? wisunPackets(f->rx) : nullptr;
}

// a mode's packetLines that needs neither the options nor the feed
template <PacketLine (*Make)()>
PacketLine plain(const Options&, const Feed&) { return Make(); }

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
    {"pocsag", "pager.message", -1, false, openPocsag, shapePocsag, nullptr},
    {"flex", "pager.message", -1, false, openFlex, shapeFlex, nullptr},
    {"acars", "acars.message", -1, true, openAcars, nullptr, nullptr},
    {"aprs", "aprs.packet", wire::Aprs, false, openAprs, shapeAprs, nullptr},
    {"ais", "ais.message", wire::Ais, true, openAis, shapeAis, plain<aisPackets>},
    {"vdl2", "vdl2.message", -1, true, openVdl2, shapeVdl2, nullptr},
    {"hfdl", "hfdl.message", -1, true, openHfdl, shapeHfdl, nullptr},
    {"rs41", "sonde.update", -1, false, openRs41, shapeRs41, nullptr},
    {"dfm", "sonde.update", -1, false, openDfm, shapeDfm, nullptr},
    {"m20", "sonde.update", -1, false, openM20, shapeM20, nullptr},
    {"ble", "ble.device", wire::Ble, false, openBle, shapeBle, nullptr},
    {"lorawan", "lora.packet", wire::LoRaWan, true, openLorawan, shapeLorawan, nullptr},
    {"zigbee", "zigbee.frame", wire::Zigbee, true, openZigbee, nullptr, plain<zigbeePackets>},
    {"thread", "thread.frame", wire::Thread, true, openThread, nullptr, plain<threadPackets>},
    {"zwave", "zwave.frame", wire::Zwave, true, openZwave, nullptr, zwavePackets},
    {"wifi", "wifi.frame", wire::Wifi, true, openWifi, nullptr, plain<wifiPackets>},
    {"wisun", "wisun.frame", wire::WiSun, true, openWisun, nullptr, wisunLines, true},
};
const size_t kBusModeCount = sizeof kBusModes / sizeof kBusModes[0];

int runBus(const BusMode& m, const Options& o, IqInput& in, Output& out) {
    Feed feed;
    std::string err;
    if (!m.open(o, &feed, &err)) { fprintf(stderr, "onair-rx: %s\n", err.c_str()); return 2; }

    // called on this thread, inside feed(): it only collects
    std::vector<automation::Event> events;
    automation::global().setTap([&events](const automation::Event& e) { events.push_back(e); }, automation::kAllFamilies);
    const PacketLine packetLine = m.packetLines ? m.packetLines(o, feed) : nullptr;
    const bool records = m.chan >= 0 && o.pcapPort;
    std::unique_ptr<HubTap> tap;   // collects from any thread, under its own lock
    if (records || (m.chan >= 0 && packetLine)) tap = std::make_unique<HubTap>(m.chan);

    RecordClock clock;
    uint64_t lines = 0, sent = 0;
    std::vector<std::vector<uint8_t>> packets;
    // what has come since the last drain: the events become lines, the packets lines (packetLine) and records, all at first, the
    // first sample of the block after which they were drained
    auto drain = [&](uint64_t first) {
        for (const automation::Event& e : events) {
            if (e.type != m.event) continue;   // not the row's event
            Json j;
            j.str("event", e.type).num("t_sample", (long long)first);
            if (m.shape && !m.shape(e, j)) continue;
            for (const auto& [k, v] : e.f) if (!j.has(k)) member(j, k, v);   // what shape wrote stands
            out.line(j.done());
            lines++;
        }
        events.clear();
        if (tap) tap->drain(packets);
        if (packetLine)
            for (const auto& p : packets) {
                Json j;
                j.str("event", m.event).num("t_sample", (long long)first);
                if (!packetLine(p.data(), p.size(), j)) continue;
                out.line(j.done());
                lines++;
            }
        if (records && !packets.empty()) {
            const int64_t t = clock.at(in, (double)first / o.rate);
            for (const auto& p : packets) {
                const std::vector<uint8_t> r = hubRecord(m.chan, p.data(), p.size());
                out.packet(t, r.data(), r.size());
            }
            sent += packets.size();
        }
        packets.clear();
    };
    std::vector<cf32> iq(1 << 14);
    uint64_t first = 0;
    for (size_t n; (n = in.read(iq)) > 0;) {
        first = in.samples() - n;
        feed(iq.data(), n);
        drain(first);
    }
    if (m.flushAtEnd) {   // what the receiver still holds, at the last block's first sample
        feed(nullptr, 0);
        drain(first);
    }
    automation::global().setTap({}, 0);
    tap.reset();
    fprintf(stderr, "onair-rx: %s: %llu lines", m.name, (unsigned long long)lines);
    if (records) fprintf(stderr, ", %llu packets", (unsigned long long)sent);
    fprintf(stderr, "\n");
    return 0;
}

} // namespace onair_rx
