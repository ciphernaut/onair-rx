// SPDX-License-Identifier: GPL-3.0-or-later
// onair-gen: one known message of a mode from OnAir's own test-signal generator, as cf32 IQ on stdout, with a second of noise
// either side (a polled mode's station: its whole broadcast). For onair-rx's tests (test/bus.sh, test/poll.sh); nothing is
// transmitted.
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
//   onair-gen --mode pocsag|flex|acars|aprs|ais|vdl2|hfdl|rs41|dfm|m20 --rate HZ [--freq HZ] [--text TEXT] [--info INFO]
//   onair-gen --mode rds --rate HZ [--ps PS] [--rt RT]
//   onair-gen --mode dmr|p25|m17 --rate HZ [--channel]
//   onair-gen --mode dvbt --rate HZ [--bandwidth 8|7|6|5] [--service NAME] [--lead SEC]
//   onair-gen --mode video --rate HZ [--deviation HZ]
//   onair-gen --mode ble|lorawan|zigbee|thread|zwave|wifi|wisun --rate HZ [--freq HZ]
//   onair-gen ... --sigmf STEM        any of the above into STEM.sigmf-data (ci16_le) and STEM.sigmf-meta instead of stdout
//   onair-gen --selftest ais-join     onair-rx's AIS sentence joiner (src/ais.cpp) on sentences made here; exit 1 on a failure
//   onair-gen --selftest sonde-id     onair-rx's sonde id spelling (src/bus.h, sondeId) on serials as OnAir writes them
//   onair-gen --selftest json-utf8    onair-rx's UTF-8 text writer (src/json.cpp, Json::utf8) on valid and broken UTF-8
//   onair-gen --selftest calls        onair-rx's call keying (src/calls.h, CallLog) on snapshots made here
//
// The known messages: POCSAG address 1234567 function 3 "HELLO ZETA" at 1200 baud (alphanumeric); FLEX capcode 1234567
// "HELLO ZETA" (alphanumeric, 1600/2); ACARS tail VH-ZET label H1 flight ZET001 ("HELLO ZETA" after the message number and
// flight); APRS VK3ABC>APRS:!2723.05S/15307.05E>ZetaSDR. --text replaces the text (APRS: the comment after the position);
// --info replaces APRS's whole information field (a message, an object, a third-party packet).
// AIS: two messages of MMSI 503000001, a position report (type 1: -27.3842, 153.1175, 12.3 kt, course 45.0 and heading 45
// degrees, under way using engine) on AIS 1, then a static report (type 5, two sentences) named ZETA TEST on AIS 2.
// VDL2: three bursts on one channel: an ACARS downlink (label H1, "HELLO ZETA" after the message number and flight) from
// aircraft 7C1234 (VH-ZET) to ground station 10A000, the station's uplink to the aircraft, and the station's GSIF to all
// stations. HFDL: ground station 1's squitter, then a logon request from aircraft 7C1234. A radiosonde (rs41, dfm, m20): one
// sonde of OnAir's scene sending its frames for 12 s, not drifting: the RS41 as serial S1234567 at -27.3842, 153.1175 (12 km,
// climbing 5 m/s); the DFM-17 (17012345) and the M20 (211-4-01234) where OnAir's scene has them (25.30, 55.50 and 25.00, 55.60).
// RDS: an FM station with PI 2AB1, PS "ZETA FM " and RT "HELLO FROM ZETASDR" for 6 s (rdsSignal); --ps and --rt replace the
// name and the text (bytes of the RDS character set). --freq, --text and --info are not taken with rds, nor --ps and --rt
// with another mode.
// The voice modes, one call each with the values of the node's own check (its rows' test): DMR, a handheld's group call in
// direct mode on time slot 1 (OnAir's single-transmission path), colour code 5, from 9990001 to talkgroup 9990, three
// superframes; P25 phase 1, a group call of NAC 0x123 from 9990001 to talkgroup 65000, three LDU1/LDU2 pairs with its header
// and terminator; M17, a voice stream (3200 bps) from VK4ZET to ALL (the broadcast address) on CAN 0, 25 frames, its voice the
// generator's known pattern (not Codec 2, so the signal is the same whether the system has the library or not). --channel
// (dmr, p25) is a channel on the air instead: 4 s of a DMR repeater's downlink (both slots, colour code 5, its traffic quiet,
// no text messages) or of a P25 control channel of NAC 0x123. A voice mode takes none of the other options.
// DVB-T: 6 s of one multiplex, 8 MHz wide (--bandwidth: 7, 6 or 5 MHz), 8K, guard 1/8, 64-QAM, code rate 3/4, with one
// television service, id 4660, named ZETA TEST (--service: the name's bytes as the SDT carries them, EN 300 468's text), and
// noise 30 dB down; --lead puts that many seconds of the noise alone before it. Video: 3 s of a PAL test card on FM, OnAir's
// FPV transmitter with the node's check swing (white 1.05 MHz above blanking: 1.5 MHz a volt) and no sound subcarrier, 30 dB
// carrier to noise; --deviation replaces the 1.05 MHz (OnAir's own FPV link is 8 MHz).
// The modes only OnAir decodes, each its generator's own scene (none of it is ZetaSDR's): Bluetooth LE, kBleSec of OnAir's
// advertisers (makeBleSynth, bleTestSchedule: a phone C0:FF:EE:12:34:56 named OnAir, an iBeacon, an Eddystone, a resolvable
// address, a scanner's scan request and connection request, an extended advertisement), the packets whitened for channels 37,
// 38 and 39 in turn, all at 0 Hz (--freq, default 2402 MHz, channel 37, is the centre the --sigmf pair records); LoRaWAN, OnAir's
// EU868 scene (makeLorawanSynth, lorawanTestSchedule and its FSK frames: uplinks, a join request and accept, a confirmed uplink
// and its answer, a 250 kHz frame, a proprietary frame) once, on its channels around --freq (default 868.3 MHz, the scene's own
// centre: the frames stay on their channels, so another --freq moves them in the band). Noise as for the messages above, its
// carrier to noise in 1 MHz for Bluetooth LE and in 125 kHz for LoRaWAN, their channels' widths.
// Four more, whose lines onair-rx makes from Wireshark hub packets: Zigbee, OnAir's schedule (makeZigbeeSynth, zigbeeTestSchedule:
// PAN 1A62, the coordinator 0000, a beacon request, a beacon, an association, an ack, a Device_annce, encrypted NWK frames);
// Thread, the frames of OnAir's Thread test (a beacon of PAN 7A31, six encrypted data frames, an MLE Discovery Request); Z-Wave,
// OnAir's EU network (makeZwaveSynth: home ID C0FFEE01, nodes 1 to 6, R1, R2 and R3 frames) around --freq (default 869.13 MHz);
// Wi-Fi, OnAir's 2.4 GHz scene (makeWifiSynth: "OnAir-Home", "OnAir-Cafe", a hidden access point, a data frame). Each once;
// carrier to noise in 2 MHz (Zigbee, Thread), 100 kHz (Z-Wave) and 20 MHz (Wi-Fi); Wi-Fi has a tenth of a second of noise
// either side, not a second (at 20 Msps a second is 160 MB). --freq of Zigbee, Thread (default 2425 MHz) and Wi-Fi (2437 MHz)
// only goes into a --sigmf pair.
// Wi-SUN, OnAir's FAN scene (makeWisunSynth, wisunTestSchedule: PAN 7A31, the border router 00124BFFFE1A2B3C, the router
// 000D6FFFFE445566 and the meter 02A1B2FFFEC3D4E5; PAN adverts, a solicit, EAPOL, PAN configurations, data and acks at 150 kbit/s,
// one PAN configuration solicit at 50 kbit/s) once, on its ANZ channels around --freq (default 921.5 MHz, the scene's own centre:
// the frames stay on their channels, so another --freq moves them in the input); carrier to noise in 400 kHz, the generator's own
// measure. stderr also says where each frame is, after the message line: "frame N from A s to B s, R bit/s on F Hz".
// --sigmf STEM writes the output as a SigMF pair instead of stdout: STEM.sigmf-data, ci16_le (each value times 32768, rounded and
// clipped), and STEM.sigmf-meta, its sample rate, the centre frequency (--freq, or the mode's default above; left out for a mode
// with neither) and an annotation per message, in the shape of iq/adsb1090.sigmf-meta.
// The channel is at 0 Hz of the output, as onair-rx expects it: the pager and packet generators put it at their mode's tune
// offset, which is cancelled here; ACARS is generated for one channel at --freq (default 131.525 MHz) with the centre there;
// AIS 1 and 2 are placed by their frequencies (161.975 and 162.025 MHz) around --freq (default 162 MHz, between the two).
// VDL2's channel is at 0 Hz; HFDL's carrier is 1440 Hz above its dial frequency, which is at 0 Hz; a sonde is moved to 0 Hz
// from where the scene has it (made at a multiple of the rate when the scene's place lies outside it, then averaged down).
//
// OnAir's generator makes the message without noise (ACARS: with its own noise 60 dB down); it is cut from its first to its
// last sample (ACARS: the burst the generator logs; a sonde: the whole 12 s), a second of silence is put either side, and noise
// is added over the whole at 25 dB carrier to noise in 25 kHz (the carrier: the mean power of the samples above -120 dB, so
// AIS's gap between its bursts, and a sonde's between its frames, do not count). stderr says where the message is: "message
// from A s to B s". A voice mode's call is sent twice, 2 s of silence between, as the node's check source repeats its
// capture: "message from A s to B s and again from C s to D s".
#include "ais.h"
#include "bus.h"
#include "calls.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/ble/ble_gen.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/fpv/fpv_gen.h"
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/m17/m17_gen.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/sonde/sonde_gen.h"
#include "dect2/source/source.h"
#include "dect2/t2/t2.h"
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/vdl2/vdl2_proto.h"
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wisun/wisun_gen.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zwave/zwave_gen.h"
#include "dect2/zwave/zwave_proto.h"
#include "dect2/zwave/zwave_rx.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace dect2;

namespace {

constexpr double kPadSec = 1.0;
constexpr double kWifiPadSec = 0.1;   // Wi-Fi's silence either side: at 20 Msps a second is 160 MB
constexpr double kRepeatGapSec = 2.0;   // a voice mode's call is sent twice, this much silence between
constexpr double kSnrDb = 25;   // carrier to noise in 25 kHz (Bluetooth LE: in 1 MHz; LoRaWAN: in 125 kHz)
constexpr size_t kChunk = 1 << 14;

int usage() {
    fprintf(stderr, "usage: onair-gen --mode pocsag|flex|acars|aprs|ais|vdl2|hfdl|rs41|dfm|m20 --rate HZ [--freq HZ] [--text TEXT] [--info INFO] > iq\n"
                    "       onair-gen --mode rds --rate HZ [--ps PS] [--rt RT] > iq\n"
                    "       onair-gen --mode dmr|p25|m17 --rate HZ [--channel] > iq\n"
                    "       onair-gen --mode dvbt --rate HZ [--bandwidth 8|7|6|5] [--service NAME] [--lead SEC] > iq\n"
                    "       onair-gen --mode video --rate HZ [--deviation HZ] > iq\n"
                    "       onair-gen --mode ble|lorawan|zigbee|thread|zwave|wifi|wisun --rate HZ [--freq HZ] > iq\n"
                    "       onair-gen ... --sigmf STEM     (STEM.sigmf-data, ci16_le, and STEM.sigmf-meta instead of stdout)\n"
                    "       onair-gen --selftest ais-join|sonde-id|json-utf8|calls\n");
    return 2;
}

// a clean generator's output from its first to its last sample that is not zero, the message followed by at least quietSec of
// zeros (at most maxSec in all)
std::vector<cf32> cut(ModeSynth& s, double rate, double quietSec, double maxSec) {
    std::vector<cf32> all, buf(kChunk);
    size_t firstNz = SIZE_MAX, lastNz = 0;
    const size_t quiet = (size_t)(quietSec * rate), most = (size_t)(maxSec * rate);
    while (all.size() < most) {
        s.generate(buf.data(), buf.size());
        for (size_t i = 0; i < buf.size(); i++)
            if (buf[i] != cf32(0.f, 0.f)) { if (firstNz == SIZE_MAX) firstNz = all.size() + i; lastNz = all.size() + i; }
        all.insert(all.end(), buf.begin(), buf.end());
        if (firstNz != SIZE_MAX && all.size() > lastNz + quiet) break;
    }
    if (firstNz == SIZE_MAX) return {};
    return std::vector<cf32>(all.begin() + (long)firstNz, all.begin() + (long)lastNz + 1);
}

std::vector<cf32> pagerSignal(double rate, bool flex, const std::string& text) {
    SynthConfig cfg;
    cfg.snrDb = -200;                               // no noise of its own
    cfg.cfoHz = pagerTuning().tuneOffsetHz;         // the generator puts the carrier this far below 0 Hz
    auto s = makePagerSynthPage(cfg, rate, flex ? kFlex1600_2 : kPocsag1200, 1234567, flex ? -1 : 3, kPagerAlpha, text);
    return s ? cut(*s, rate, 0.25, 120) : std::vector<cf32>{};
}

std::vector<cf32> aprsSignal(double rate, const std::string& info) {
    ax25::Frame f;
    f.to.call = "APRS";
    f.from.call = "VK3ABC";
    f.info = info;
    PacketGenFrame g;
    g.frame = f;
    g.baud = 1200;
    SynthConfig cfg;
    cfg.snrDb = -200;
    cfg.cfoHz = packetTuning().tuneOffsetHz;
    auto s = makePacketSynthBurst(cfg, rate, {g}, 0);
    return s ? cut(*s, rate, 0.1, 2 * packetSynthBurstSec({g}, 0) + 1) : std::vector<cf32>{};
}

std::vector<cf32> acarsSignal(double rate, double freq, const std::string& text) {
    AcarsGenOptions o;
    o.aircraft = 1;
    o.rateFactor = 1e-4;   // the next downlink is hours away
    o.centerHz = freq;
    o.channelsHz = {freq};
    o.cfoSpreadHz = 0;
    o.uplinks = false;
    o.reg = "VH-ZET";
    o.text = "M01AZET001" + text;   // a downlink's text starts with its message number and flight
    o.log = std::make_shared<AcarsSentLog>();
    SynthConfig cfg;
    cfg.snrDb = 60;
    auto s = makeAcarsSynthEx(o, cfg, rate);
    if (!s) return {};
    std::vector<cf32> all, buf(kChunk);
    while (all.size() < (size_t)(30 * rate)) {
        s->generate(buf.data(), buf.size());
        all.insert(all.end(), buf.begin(), buf.end());
        std::lock_guard<std::mutex> lk(o.log->mu);
        if (!o.log->sent.empty() && o.log->sent[0].endSec * rate <= (double)all.size()) {
            const size_t a = (size_t)(o.log->sent[0].startSec * rate), b = (size_t)(o.log->sent[0].endSec * rate);
            return std::vector<cf32>(all.begin() + (long)a, all.begin() + (long)b);
        }
    }
    return {};
}

// --- AIS: the payloads (ITU-R M.1371-5, the layouts of OnAir's own generator, ais_gen.cpp), then OnAir's burst renderer
constexpr uint32_t kAisMmsi = 503000001;
ais::Bits aisPosition() {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 1);
    putU(b, 8, 30, kAisMmsi);
    putU(b, 38, 4, 0);                                        // under way using engine
    putS(b, 42, 8, -128);                                     // no rate of turn
    putU(b, 50, 10, 123);                                     // 12.3 kt
    putU(b, 60, 1, 1);
    putS(b, 61, 28, (int32_t)std::lround(153.1175 * 600000));
    putS(b, 89, 27, (int32_t)std::lround(-27.3842 * 600000));
    putU(b, 116, 12, 450);                                    // 45.0 degrees
    putU(b, 128, 9, 45);
    putU(b, 137, 6, 60);                                      // no time stamp
    b.resize(168, 0);
    return b;
}
ais::Bits aisStatic() {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 5);
    putU(b, 8, 30, kAisMmsi);
    putText(b, 70, 42, "");
    putText(b, 112, 120, "ZETA TEST");
    putU(b, 270, 4, 1);                                       // GPS
    putU(b, 283, 5, 24); putU(b, 288, 6, 60);                 // no arrival time
    putText(b, 302, 120, "");
    b.resize(424, 0);
    return b;
}
std::vector<cf32> aisSignal(double rate, double freq) {
    AisRenderConfig c;
    c.rate = rate;
    c.durationSec = 0.3;
    c.snrDb = -200;                    // no noise of its own
    c.tuneOffsetHz = freq - 162e6;     // AIS 1 and 2 at their frequencies around freq
    const std::vector<cf32> all = renderAisBursts({{aisPosition(), 'A', 0.01}, {aisStatic(), 'B', 0.15}}, c);
    size_t a = 0, b = all.size();
    while (a < b && all[a] == cf32(0.f, 0.f)) a++;
    while (b > a && all[b - 1] == cf32(0.f, 0.f)) b--;
    return std::vector<cf32>(all.begin() + (long)a, all.begin() + (long)b);
}

// from the first to the last sample of x that is not zero
std::vector<cf32> trim(const std::vector<cf32>& x) {
    size_t a = 0, b = x.size();
    while (a < b && x[a] == cf32(0.f, 0.f)) a++;
    while (b > a && x[b - 1] == cf32(0.f, 0.f)) b--;
    return std::vector<cf32>(x.begin() + (long)a, x.begin() + (long)b);
}

// --- VDL2: the frames from OnAir's AVLC and ACARS builders (vdl2_proto.h), each a burst of OnAir's renderer, 0.1 s apart
constexpr uint32_t kAircraft = 0x7C1234, kVdl2Ground = 0x10A000;
std::vector<cf32> vdl2Signal(double rate, const std::string& text) {
    const Vdl2Addr air{kAircraft, 1, false}, ground{kVdl2Ground, 4, false}, everyone{0xFFFFFF, 7, false};
    const std::vector<Vdl2Frames> bursts = {
        {vdl2Frame(ground, air, vdl2ControlI(0, 0, false), vdl2AcarsInfo("VH-ZET", "H1", '0', "M01AZET001" + text))},
        {vdl2Frame(air, ground, vdl2ControlI(0, 1, false), vdl2AcarsInfo("VH-ZET", "C1", 'A', text))},
        {vdl2Frame(everyone, ground, kVdl2CtrlXID, vdl2XidGsif(-27.3842, 153.1175, "YBBN"))},
    };
    std::vector<Vdl2Wave> waves;
    double t = 0.01;
    for (const Vdl2Frames& f : bursts) {
        Vdl2Wave w = vdl2MakeWave(f);
        w.start = t * rate;
        w.carHz = 0;
        waves.push_back(w);
        t += vdl2BurstSeconds(f) + 0.1;
    }
    std::vector<cf32> all((size_t)(t * rate), cf32(0.f, 0.f));
    for (const Vdl2Wave& w : waves) vdl2Render(w, all.data(), 0, all.size(), rate);
    return trim(all);
}

// --- HFDL: ground station 1's squitter, then a logon request from the aircraft (OnAir's PDU builders, hfdl_proto.h), each a
// burst of OnAir's renderer at the data rate OnAir's traffic sends it with (or the next that carries it), 0.2 s apart
std::vector<cf32> hfdlSignal(double rate) {
    constexpr int kGs = 1;
    const HfdlStationInfo* st = hfdlStation(kGs);
    HfdlSquitterSpec sq;
    sq.gsId = kGs;
    sq.mask = st ? (uint32_t)((1u << st->khz.size()) - 1) : 1u;
    const std::vector<std::pair<int, std::vector<uint8_t>>> bursts = {
        {1, hfdlSpdu(sq)}, {0, hfdlMpduDown(kGs, 0, {hfdlLogonRequest(kAircraft)})}};
    std::vector<HfdlWave> waves;
    double t = 0.01;
    for (const auto& [rate1, pdu] : bursts) {
        int m1 = rate1;
        while (m1 < 3 && (int)pdu.size() > hfdlPayloadOctets(m1)) m1++;
        HfdlWave w = hfdlMakeWave(m1, pdu);
        w.start = t * rate;
        w.carHz = kHfdlCarrierHz;
        waves.push_back(w);
        t += hfdlBurstSeconds(m1) + 0.2;
    }
    std::vector<cf32> all((size_t)(t * rate), cf32(0.f, 0.f));
    for (const HfdlWave& w : waves) hfdlRender(w, all.data(), 0, all.size(), rate);
    return trim(all);
}

// --- a radiosonde: one sonde of OnAir's scene (sonde_gen.h), its frames for kSondeSec, moved to 0 Hz. The scene leaves out a
// sonde further than 0.45 of the rate from its centre, so such a one is made at k times the rate and k samples are averaged
// (the sonde, about 10 kHz wide, is alone in the band)
constexpr double kSondeSec = 12;
std::vector<cf32> sondeSignal(double rate, const std::string& type) {
    SynthConfig c;
    c.mode = 15;
    c.snrDb = 200;            // no noise of its own
    c.modeOpt[1] = 1;         // one sonde
    c.modeVal[0] = 1e-9;      // no drift: the position stays
    std::unique_ptr<ModeSynth> s;
    int k = 1;
    if (type == "rs41") {
        s = makeSondeSynthEx(c, rate, "S1234567", -27.3842, 153.1175);
    } else {
        c.modeOpt[0] = type == "dfm" ? 2 : 8;                      // the scene's DFM-17 at 402.2 MHz, its M20 at 401.6 MHz
        const double offset = type == "dfm" ? -800e3 : -1400e3;    // from the scene's centre, 403 MHz
        while (std::fabs(offset) > 0.45 * k * rate) k++;
        c.cfoHz = -offset;
        s = makeSondeSynth(c, k * rate);
    }
    if (!s) return {};
    std::vector<cf32> out((size_t)(kSondeSec * rate)), buf(kChunk * (size_t)k);
    for (size_t at = 0; at < out.size();) {
        s->generate(buf.data(), buf.size());
        for (size_t i = 0; i < buf.size() && at < out.size(); i += (size_t)k, at++) {
            cf32 v(0.f, 0.f);
            for (int j = 0; j < k; j++) v += buf[i + (size_t)j];
            out[at] = v / (float)k;
        }
    }
    return out;
}

// --- RDS: a station of OnAir's FM generator (fm_gen.h) for kFmSec at 0 Hz, with the generator's own stereo tones and its own
// noise (40 dB carrier to noise in 200 kHz). A broadcast never stops, so there is no silence around it: the whole output is
// the message. ps and rt are bytes of the RDS character set (EBU Latin above 0x7f: 0x80 is a, 0x82 e with an acute); the
// generator pads the name to 8 bytes and ends the text with a carriage return
constexpr double kFmSec = 6;
std::vector<cf32> rdsSignal(double rate, const std::string& ps, const std::string& rt) {
    FmGenConfig c;
    c.rate = rate;
    c.pi = 0x2AB1;
    c.ps = ps;
    c.rt = rt;
    FmGenerator g(c);
    std::vector<cf32> out((size_t)(kFmSec * rate));
    g.generate(out.data(), out.size());
    return out;
}

// --- DVB-T: kDvbtSec of one multiplex of OnAir's generator (dvbt_gen.h), 8K, guard 1/8, 64-QAM, code rate 3/4, not
// hierarchical, its transport stream testTsSourceNamed's (the PAT, PMT and SDT of one television service, kDvbtService, and
// some private data). It is made at the width's native rate (64/7 MHz for 8 MHz) and resampled to the rate, with noise 30 dB
// below the multiplex in its 7.6 MHz (scaled with the width). service: the service name as the SDT carries it (EN 300 468's
// text: a first byte below 0x20 names the character table). leadSec: that much of the noise alone first, so that the
// multiplex comes on the air there (the receiver's lock then lands where a test wants it). A broadcast never stops: the
// whole output is the message
constexpr double kDvbtSec = 6;
constexpr int kDvbtService = 4660;
constexpr double kDvbtSnrDb = 30;
std::vector<cf32> dvbtSignal(double rate, int bwMhz, const std::string& service, double leadSec) {
    dvbt::Params p;
    p.mode = dvbt::k8K;
    p.guard = dvbt::kGi8;
    p.mod = dvbt::k64Qam;
    p.crHp = p.crLp = dvbt::kR34;
    dvbt::Generator g(p, dvbt::testTsSourceNamed(1, kDvbtService, service));
    ExactResampler rs;
    if (!rs.configure(nativeRateHz(bwMhz), rate)) return {};
    const size_t want = (size_t)std::llround(kDvbtSec * rate);
    std::vector<cf32> sym, out;
    while (out.size() < want) {
        g.nextSymbol(sym);
        rs.process(sym.data(), sym.size(), out);
    }
    out.resize(want);
    double power = 0;
    for (const cf32& v : out) power += std::norm(v);
    out.insert(out.begin(), (size_t)std::llround(leadSec * rate), cf32(0.f, 0.f));
    const double band = 7.61e6 * bwMhz / 8;   // the 6817 carriers of 8K
    genutil::NoiseSource noise(7);
    noise.add(out.data(), out.size(), (float)std::sqrt(power / (double)want * rate / (2 * band * std::pow(10.0, kDvbtSnrDb / 10))));
    return out;
}

// --- analog video: kVideoSec of OnAir's FPV generator (fpv_gen.h), a PAL test card frequency modulated at 0 Hz with the swing
// of the node's own check (video_decode's test: 1.5 MHz a volt, so white 0.7 V is 1.05 MHz above blanking; FpvGenConfig::devHz
// is white's offset, the sync tip 0.4 devHz below blanking) and, like it, no sound subcarrier; 30 dB carrier to noise in
// 17 MHz (the generator's default). devHz (not 0) replaces the swing: 8 MHz is the generator's own, an FPV link's. A link that
// stays up: the whole output is the message
constexpr double kVideoSec = 3;
constexpr double kVideoDevHz = 1.05e6;
std::vector<cf32> videoSignal(double rate, double devHz) {
    FpvGenConfig c;
    c.rate = rate;
    c.devHz = devHz > 0 ? devHz : kVideoDevHz;
    c.sound = 0;
    FpvGenerator g(c);
    std::vector<cf32> out((size_t)std::llround(kVideoSec * rate));
    for (size_t at = 0; at < out.size(); at += 65536) g.generate(out.data() + at, std::min<size_t>(65536, out.size() - at));
    return out;
}

// --- the voice modes: one call of OnAir's generator for the mode, without noise, made for kVoiceSec (the call and the
// generator's silence after it) and cut to the call. The values are the node's check values (its rows' test)
constexpr double kVoiceSec = 3;
constexpr double kChannelSec = 4;   // --channel: a repeater's downlink or a control channel this long
constexpr uint32_t kVoiceSrc = 9990001;
template <class G>
std::vector<cf32> voiceCut(G& g, double rate, double sec = kVoiceSec) {
    std::vector<cf32> all((size_t)(sec * rate));
    g.generate(all.data(), all.size());
    return trim(all);
}
// DMR: OnAir's single transmission (dmr_gen.h, txEnable), a handheld in direct mode on time slot 1; with channel, a
// repeater's downlink instead, both slots continuous, its traffic quiet and no text messages
std::vector<cf32> dmrSignal(double rate, bool channel) {
    DmrGenConfig c;
    c.rate = rate;
    c.cc = 5;
    c.snrDb = 1000;        // no noise of its own: its level underflows to exactly 0, so the silence around the call is zeros
    if (channel) {
        c.traffic = 1;
        c.textMessages = false;
        DmrSignal g(c);
        std::vector<cf32> all((size_t)(kChannelSec * rate));
        g.generate(all.data(), all.size());
        return all;
    }
    c.txEnable = true;
    c.txCallType = 0;      // group voice
    c.txSrc = kVoiceSrc;
    c.txDst = 9990;
    c.txSlot = 1;
    c.txMobile = false;    // direct-mode sync patterns, which name the time slot
    c.txVoiceSuper = 3;
    DmrSignal g(c);
    return voiceCut(g, rate);
}
// P25 phase 1: one group call (p25_gen.h), header, three LDU1/LDU2 pairs (0.36 s each) and terminator, played once; with
// channel, a trunked site's control channel of the same NAC for kChannelSec instead
std::vector<cf32> p25Signal(double rate, bool channel) {
    P25GenConfig c;
    c.rate = rate;
    c.snrDb = 99;          // no noise of its own
    c.loop = false;
    P25GenTx t;
    t.kind = P25GenTx::Voice;
    t.nac = 0x123;
    t.src = (int)kVoiceSrc;
    t.dst = 65000;
    t.group = true;
    t.durSec = 3 * 0.36;
    if (channel) {
        t.kind = P25GenTx::Control;
        t.durSec = kChannelSec;
    }
    c.txs = {t};
    P25Generator g(c);
    return voiceCut(g, rate, channel ? kChannelSec + 1 : kVoiceSec);
}
// M17: one voice stream (m17_gen.h), 25 frames of 40 ms, its voice the known pattern (voice false); "@ALL" is OnAir's
// spelling of the broadcast address, which the node spells ALL
std::vector<cf32> m17Signal(double rate) {
    M17GenConfig c;
    c.rate = rate;
    c.snrDb = 99;          // no noise of its own
    c.loop = false;
    c.voice = false;
    M17GenTx t;
    t.src = "VK4ZET";
    t.dst = "@ALL";
    t.can = 0;
    t.dataType = 2;        // voice, 3200 bps
    t.durSec = 25 * 0.04;
    c.txs = {t};
    M17Generator g(c);
    return voiceCut(g, rate);
}

// --- Bluetooth LE: kBleSec of OnAir's advertising scene (ble_gen.h, makeBleSynth), without noise. The scene puts the channel
// tuneOffsetHz below 0 Hz (the radio sits that far above it); the offset is cancelled, so the channel is at 0 Hz
constexpr double kBleSec = 0.25;   // about six rounds of the scene's eleven packets
std::vector<cf32> bleSignal(double rate) {
    SynthConfig c;
    c.snrDb = 200;                        // no noise of its own (99 and up)
    c.cfoHz = bleTuning().tuneOffsetHz;
    auto s = makeBleSynth(c, rate);
    if (!s) return {};
    std::vector<cf32> all((size_t)std::llround(kBleSec * rate));
    s->generate(all.data(), all.size());
    return trim(all);
}

// --- LoRaWAN: OnAir's EU868 scene (lorawan_gen.h, makeLorawanSynth) once, without noise, freq at 0 Hz. The scene centres its
// dial (lorawanPlanCentreMhz, 868.3 MHz) tuneOffsetHz below 0 Hz; the carrier offset moves it to freq - 868.3 MHz instead. It is
// made for as long as its frames last (the LoRa frames' and the FSK frames' own lengths), before the scene would loop
std::vector<cf32> lorawanSignal(double rate, double freq) {
    SynthConfig c;
    c.snrDb = 200;                        // no noise of its own (99 and up)
    c.modeOpt[0] = lorawan::EU868;
    c.cfoHz = lorawanTuning().tuneOffsetHz + (lorawanPlanCentreMhz(lorawan::EU868) * 1e6 - freq);
    auto s = makeLorawanSynth(c, rate);
    if (!s) return {};
    const double sec = std::max(lorawanSceneSeconds(lorawanTestSchedule(lorawan::EU868)),
                                lorawanFskSceneSeconds(lorawanFskTestSchedule(lorawan::EU868)));
    std::vector<cf32> all((size_t)std::llround(sec * rate));
    s->generate(all.data(), all.size());
    return trim(all);
}

// --- the 802.15.4 scenes (Zigbee, Thread): OnAir's frames (zigbee_gen.h) once, without noise, the channel at 0 Hz. The generator
// dials the channel zigbeeTuning().tuneOffsetHz below 0 Hz (the radio sits that far above it), which the carrier offset cancels. A
// scene is made for as long as its frames last (the chip count and the gap of ZigbeeGenerator's own refill), before it would loop
size_t zigbeeSceneSamples(const std::vector<ZigbeeGenFrame>& v, double rate, double leadSec) {
    const double perSample = 2e6 / rate;   // chips per sample
    size_t n = (size_t)std::llround(leadSec * rate);
    for (const ZigbeeGenFrame& f : v) {
        const size_t chips = (size_t)(f.preambleSyms + 4 + 2 * std::min<size_t>(f.psdu.size(), 127)) * 32;
        n += (size_t)std::ceil(((double)chips + 8) / perSample) + (size_t)std::llround(f.gapSec * rate);
    }
    return n;
}
std::vector<cf32> zigbeeSignal(double rate) {
    SynthConfig c;
    c.snrDb = 200;                        // no noise of its own (99 and up)
    c.cfoHz = zigbeeTuning().tuneOffsetHz;
    auto s = makeZigbeeSynth(c, rate);
    if (!s) return {};
    std::vector<cf32> all(zigbeeSceneSamples(zigbeeTestSchedule(), rate, 0.0005));
    s->generate(all.data(), all.size());
    return trim(all);
}

// Thread: the frames of OnAir's own Thread test (tests/thread/test_thread_rx.cpp): a Thread beacon of the network "HomeThread" (PAN
// 7A31, 5000), six link-layer encrypted data frames between 5400 or 5401 and 5000, and an MLE Discovery Request in the clear from the
// extended address 0123456789ABCDEF
std::vector<ZigbeeGenFrame> threadSchedule() {
    std::vector<ZigbeeGenFrame> v;
    auto add = [&](const std::vector<uint8_t>& psdu) { ZigbeeGenFrame f; f.psdu = psdu; f.gapSec = 0.0008; v.push_back(f); };
    {   // beacon
        std::vector<uint8_t> pl = {0xFF, 0xCF, 0x00, 0x00, 0x03, 0x20};
        const char* nm = "HomeThread";
        for (int i = 0; i < 16; i++) pl.push_back(i < 10 ? (uint8_t)nm[i] : 0);
        for (int i = 0; i < 8; i++) pl.push_back((uint8_t)(0x11 * (i + 1)));
        zigbee::MacBuild b; b.type = zigbee::Beacon; b.destMode = 0; b.srcShort = 0x5000; b.srcPan = 0x7A31; b.payload = pl;
        add(zigbee::buildMac(b));
    }
    for (int k = 0; k < 6; k++) {   // encrypted data between two nodes of the PAN
        std::vector<uint8_t> pl = {0x0D, (uint8_t)k, 0, 0, 0, 0x01};
        for (int i = 0; i < 24; i++) pl.push_back((uint8_t)(i * 13 + k));
        zigbee::MacBuild b; b.type = zigbee::Data; b.destPan = 0x7A31; b.destShort = 0x5000; b.srcShort = (uint16_t)(0x5400 + k % 2);
        b.secured = true; b.seq = (uint8_t)k; b.payload = pl;
        add(zigbee::buildMac(b));
    }
    {   // MLE Discovery Request in the clear
        zigbee::MacBuild b; b.type = zigbee::Data; b.destPan = 0xFFFF; b.destShort = 0xFFFF; b.srcShort = 0x5600; b.srcMode = 3;
        b.srcExt = 0x0123456789ABCDEFULL;
        b.payload = {0x7F, 0x3B, 0x02, 0xF0, 0x4D, 0x4C, 0x4D, 0x4C, 0x12, 0x34, 0xFF, 0x10, 0x0B, 0x02, 0x20, 0x00};
        add(zigbee::buildMac(b));
    }
    return v;
}
std::vector<cf32> threadSignal(double rate) {
    ZigbeeGenConfig c;
    c.rate = rate;
    c.snrDb = 200;                        // no noise of its own
    c.dialOffsetHz = 0;                   // the channel at 0 Hz
    c.frames = threadSchedule();
    ZigbeeGenerator g(c);
    std::vector<cf32> all(zigbeeSceneSamples(c.frames, rate, c.leadSec));
    g.generate(all.data(), all.size());
    return trim(all);
}

// --- Z-Wave: OnAir's EU network (zwave_gen.h, makeZwaveSynth: home ID C0FFEE01, controller 1, nodes 2 to 6; R1, R2 and R3 frames
// on the EU channels) once, without noise. The scene centres its dial (the region's centre, 869.13 MHz for EU) at 0 Hz; the carrier
// offset moves it to freq - 869.13 MHz instead, so that --freq is at 0 Hz. Made for as long as its frames last
constexpr const char* kZwaveRegion = "EU";
std::vector<cf32> zwaveSignal(double rate, double freq) {
    const std::vector<zwave::Region>& regs = zwave::regions();
    size_t ri = 0;
    while (regs[ri].id != std::string(kZwaveRegion)) ri++;
    SynthConfig c;
    c.snrDb = 200;
    c.modeOpt[0] = (int)ri;
    c.cfoHz = zwaveTuning().tuneOffsetHz + (regs[ri].centreMhz * 1e6 - freq);
    auto s = makeZwaveSynth(c, rate);
    if (!s) return {};
    size_t n = 0;
    for (const ZwGenFrame& f : zwaveTestSchedule(kZwaveRegion)) {
        const size_t chips = zwavePpduBits(f).size() * (f.rate == 1 ? 2 : 1) + 2;
        n += (size_t)((double)chips * (rate / zwave::chipRate(f.rate))) + (size_t)(f.gapSec * rate);
    }
    std::vector<cf32> all(n);
    s->generate(all.data(), all.size());
    return trim(all);
}

// --- Wi-Fi: OnAir's 2.4 GHz scene (wifi_gen.h, makeWifiSynth: two access points, "OnAir-Home" and "OnAir-Cafe", a hidden one, a
// probe response and a data frame, 1 Mbit/s DSSS on channel 6) once, without noise, the channel at 0 Hz
std::vector<cf32> wifiSignal(double rate) {
    SynthConfig c;
    c.snrDb = 200;
    auto s = makeWifiSynth(c, rate);
    if (!s) return {};
    const double perSample = wifi::kChipRate / rate;
    size_t n = (size_t)std::llround(0.001 * rate);   // the generator's lead
    for (const WifiGenFrame& f : wifiTestSchedule())
        n += (size_t)std::ceil((double)(wifi::ppduBits(f.mpdu).size() * 11) / perSample) + (size_t)std::llround(f.gapSec * rate);
    std::vector<cf32> all(n);
    s->generate(all.data(), all.size());
    return trim(all);
}

// --- the output: cf32 on stdout, or with a stem the SigMF pair: <stem>.sigmf-data in ci16_le (each value times 32768, rounded and
// clipped) and <stem>.sigmf-meta (the rate, the centre when known, one annotation per message: its first sample and its length).
// false with the reason on stderr when it could not be written
struct Span { size_t start, count; };
std::string number(double v) {   // a whole number without a decimal point, as the SigMF files of OnAir and ZetaSDR have it
    char b[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%.0f", v);
    else snprintf(b, sizeof b, "%.17g", v);
    return b;
}
// --- Wi-SUN: OnAir's FAN scene (wisun_gen.h, makeWisunSynth) once, without noise. The scene centres its channels' reference
// (wisunTuning().defMhz, 921.5 MHz) at 0 Hz; the carrier offset moves it to freq - 921.5 MHz instead, so that freq is at 0 Hz and
// every frame stays on its channel. Made for as long as its frames last (WisunGenerator's refill: the lead, then each frame and
// its gap), before the scene would loop. frames: where each frame is in the result, with its bit rate and its channel (Hz)
struct WisunSpan { size_t start, count; int bitRate; double hz; };
std::vector<cf32> wisunSignal(double rate, double freq, std::vector<WisunSpan>* frames) {
    SynthConfig c;
    c.snrDb = 200;                        // no noise of its own (99 and up)
    c.cfoHz = wisunTuning().defMhz * 1e6 - freq;
    auto s = makeWisunSynth(c, rate);
    if (!s) return {};
    // the schedule makeWisunSynth plays (wisun_gen.cpp, WisunSynth): its channels within 0.4 of the rate around 921.5 MHz, at most 6 MHz
    const std::vector<WisunGenFrame> sched = wisunTestSchedule(wisunTuning().defMhz, std::min(6.0, 0.4 * rate / 1e6));
    const WisunGenConfig defaults;
    size_t n = (size_t)std::llround(defaults.leadSec * rate);
    std::vector<WisunSpan> at;
    for (const WisunGenFrame& f : sched) {
        const size_t len = (size_t)std::ceil((double)(wisunPpduBits(f).size() + 1) / f.bitRate * rate);
        at.push_back({n, len, f.bitRate, f.mhz * 1e6});
        n += len + (size_t)std::llround(f.gapSec * rate);
    }
    std::vector<cf32> all(n);
    s->generate(all.data(), all.size());
    size_t a = 0;   // where trim() starts the result: the spans move with it
    while (a < all.size() && all[a] == cf32(0.f, 0.f)) a++;
    for (WisunSpan& w : at) {
        const size_t end = w.start + w.count;
        w.start = w.start > a ? w.start - a : 0;
        w.count = end - a - w.start;
    }
    *frames = at;
    return trim(all);
}

bool writeOut(const std::vector<cf32>& x, const std::string& stem, double rate, double centre, const std::vector<Span>& msgs) {
    if (stem.empty()) {
        if (fwrite(x.data(), sizeof(cf32), x.size(), stdout) != x.size() || fflush(stdout) != 0) {
            fprintf(stderr, "onair-gen: stdout: %s\n", strerror(errno));
            return false;
        }
        return true;
    }
    const std::string data = stem + ".sigmf-data", meta = stem + ".sigmf-meta";
    FILE* f = fopen(data.c_str(), "wb");
    if (!f) { fprintf(stderr, "onair-gen: %s: %s\n", data.c_str(), strerror(errno)); return false; }
    std::vector<int16_t> buf;
    bool ok = true;
    for (size_t at = 0; at < x.size() && ok; at += kChunk) {
        const size_t n = std::min(kChunk, x.size() - at);
        buf.resize(2 * n);
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[at + i];
            buf[2 * i] = (int16_t)std::clamp<long>(std::lround(v.real() * 32768.0), -32768, 32767);
            buf[2 * i + 1] = (int16_t)std::clamp<long>(std::lround(v.imag() * 32768.0), -32768, 32767);
        }
        ok = fwrite(buf.data(), sizeof(int16_t), buf.size(), f) == buf.size();
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { fprintf(stderr, "onair-gen: %s: %s\n", data.c_str(), strerror(errno)); return false; }
    std::string m = "{\n  \"annotations\": [";
    for (size_t i = 0; i < msgs.size(); i++)
        m += std::string(i ? "," : "") + "\n    {\n      \"core:label\": \"message\",\n      \"core:sample_count\": " +
             std::to_string(msgs[i].count) + ",\n      \"core:sample_start\": " + std::to_string(msgs[i].start) + "\n    }";
    m += msgs.empty() ? "],\n" : "\n  ],\n";
    m += "  \"captures\": [\n    {\n";
    if (centre > 0) m += "      \"core:frequency\": " + number(centre) + ",\n";
    m += "      \"core:sample_start\": 0\n    }\n  ],\n";
    m += "  \"global\": {\n    \"core:datatype\": \"ci16_le\",\n    \"core:sample_rate\": " + number(rate) +
         ",\n    \"core:version\": \"1.0.0\"\n  }\n}\n";
    f = fopen(meta.c_str(), "wb");
    const bool wrote = f && fwrite(m.data(), 1, m.size(), f) == m.size();
    if (!f || fclose(f) != 0 || !wrote) {
        fprintf(stderr, "onair-gen: %s: %s\n", meta.c_str(), strerror(errno));
        return false;
    }
    return true;
}

// --- the joiner's own test: sentences in as the hub has them (with their line ends), messages out
int selftestAisJoin() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* what) { printf("%s: %s\n", ok ? "ok" : "FAIL", what); fails += !ok; };
    using Msg = std::optional<std::vector<std::string>>;
    const std::vector<std::string> pos = ais::toNmea(aisPosition(), 'A', 0), st3 = ais::toNmea(aisStatic(), 'A', 3),
                                   st4 = ais::toNmea(aisStatic(), 'A', 4), stB = ais::toNmea(aisStatic(), 'B', 3);
    if (pos.size() != 1 || st3.size() != 2 || st4.size() != 2 || stB.size() != 2) { printf("FAIL: OnAir's sentences\n"); return 1; }
    auto in = [](onair_rx::AisJoiner& j, const std::string& s) { return j.add(s + "\r\n"); };
    {
        onair_rx::AisJoiner j;
        const Msg m = in(j, pos[0]);
        check(m && *m == pos, "one sentence is a message, without its line end");
    }
    {
        onair_rx::AisJoiner j;
        const Msg a = in(j, st3[0]), b = in(j, st3[1]);
        check(!a && b && *b == st3, "two sentences are a message once the second comes");
        onair_rx::Json line;
        check(b && onair_rx::aisLine(*b, line) && line.done().find("\"msg_type\":5") != std::string::npos &&
                  line.done().find("\"name\":\"ZETA TEST\"") != std::string::npos,
              "its line is the static report of ZETA TEST");
    }
    {
        onair_rx::AisJoiner j;
        const Msg a = in(j, st3[0]), p = in(j, pos[0]), b = in(j, st3[1]);
        check(!a && p && *p == pos && !b, "a first part whose second never comes is forgotten at the channel's next message");
    }
    {
        onair_rx::AisJoiner j;
        const Msg a = in(j, st4[0]), b = in(j, st3[1]), c = in(j, st4[1]);
        check(!a && !b && c && *c == st4, "part 2 of sequence id 3 is never joined to part 1 of sequence id 4");
    }
    {
        onair_rx::AisJoiner j;
        const Msg a = in(j, stB[0]), p = in(j, pos[0]), b = in(j, stB[1]);
        check(!a && p && b && *b == stB, "a message on AIS 1 does not end one open on AIS 2 (same sequence id)");
    }
    {
        onair_rx::AisJoiner j;
        const Msg a = in(j, st3[1]), b = in(j, st3[0]), c = in(j, st3[0]), d = in(j, st3[1]);
        check(!a && !b && !c && d && *d == st3, "a second part alone is no message; a first part again starts it again");
    }
    {
        onair_rx::AisJoiner j;
        std::string bad = pos[0];
        bad[bad.size() - 8] = bad[bad.size() - 8] == '0' ? '1' : '0';   // a payload character: the checksum no longer fits
        check(!in(j, bad), "a sentence whose checksum is wrong is nothing");
    }
    return fails ? 1 : 0;
}

// --- a sonde's id (bus.h's sondeId) as rs1729's decoders spell it, from serials as OnAir's decoders write them
int selftestSondeId() {
    int fails = 0;
    auto check = [&fails](const std::string& type, const std::string& serial, const std::string& want) {
        const std::string got = onair_rx::sondeId(type, serial);
        printf("%s: %s \"%s\" is \"%s\"", got == want ? "ok" : "FAIL", type.c_str(), serial.c_str(), got.c_str());
        if (got != want) printf(", want \"%s\"", want.c_str());
        printf("\n");
        fails += got != want;
    };
    check("RS41", "S1234567", "S1234567");
    check("M20", "611-2-01234", "M20-611-2-01234");
    check("M20", "M20-611-2-01234", "M20-611-2-01234");
    check("DFM", "17012345", "DFM-17012345");      // DFM-17, "%u": no padding past 6 digits
    check("DFM", "123456", "DFM-123456");
    check("DFM", "12345", "DFM- 12345");           // dfm09mod's "%6u": space-padded
    check("DFM", "0A1B2C", "DFM- A1B2C");          // DFM-06: OnAir's "%06X", dfm09mod's "%6X"
    check("DFM", "4294967295", "DFM-429496729");   // dfm09mod copies at most 9 characters
    return fails ? 1 : 0;
}

// --- the JSON writer's UTF-8 text (src/json.cpp, Json::utf8): valid UTF-8 as it is, the rest as Json::str writes it
int selftestJsonUtf8() {
    int fails = 0;
    auto check = [&fails](const std::string& v, const std::string& want, const char* what) {
        const std::string got = onair_rx::Json().utf8("t", v).done();
        const std::string w = "{\"t\":\"" + want + "\"}";
        printf("%s: %s: %s", got == w ? "ok" : "FAIL", what, got.c_str());
        if (got != w) printf(", want %s", w.c_str());
        printf("\n");
        fails += got != w;
    };
    check("ZETA FM ", "ZETA FM ", "ASCII as it is");
    check("R\xc3\xa1" "DIO \xe2\x82\xac \xf0\x9f\x93\xbb", "R\xc3\xa1" "DIO \xe2\x82\xac \xf0\x9f\x93\xbb", "two-, three- and four-byte characters as they are");
    check("a\"b\\c\x01\x7f", "a\\\"b\\\\c\\u0001\\u007f", "a quote, a backslash and control characters escaped");
    check("R\xe1" "DIO", "R\\u00e1DIO", "a Latin-1 byte: not UTF-8, so byte by byte");
    check("\xc3", "\\u00c3", "a character cut short");
    check("\xc0\xaf", "\\u00c0\\u00af", "an overlong form");
    check("\xed\xa0\x80", "\\u00ed\\u00a0\\u0080", "a surrogate");
    check("\xf4\x90\x80\x80", "\\u00f4\\u0090\\u0080\\u0080", "past U+10FFFF");
    check("\x80" "a", "\\u0080a", "a continuation byte first");
    return fails ? 1 : 0;
}

// --- the call keying (src/calls.h, CallLog): successive snapshots in, lines out
int selftestCalls() {
    using onair_rx::CallLog;
    using onair_rx::CallSeen;
    using onair_rx::Json;
    int fails = 0;
    auto members = [](long long src, const char* alias = nullptr) {
        Json j;
        j.num("src", src);
        if (alias) j.str("alias", alias);
        return j;
    };
    // the lines of one poll of these entries, each ended by a line end
    auto see = [](CallLog& log, const std::vector<CallSeen>& now) {
        std::vector<Json> lines;
        log.see(now, lines);
        std::string s;
        for (const Json& l : lines) s += l.done() + "\n";
        return s;
    };
    auto finish = [](CallLog& log) {
        std::vector<Json> lines;
        log.finish(lines);
        std::string s;
        for (const Json& l : lines) s += l.done() + "\n";
        return s;
    };
    auto check = [&fails](const std::string& got, const std::string& want, const char* what) {
        printf("%s: %s\n", got == want ? "ok" : "FAIL", what);
        if (got != want) printf("  got:\n%s  want:\n%s", got.c_str(), want.c_str());
        fails += got != want;
    };
    const std::string a = "{\"src\":1,\"eot\":false}\n", aEnd = "{\"src\":1,\"eot\":true}\n";
    const std::string aAlias = "{\"src\":1,\"alias\":\"Z\",\"eot\":false}\n", aAliasEnd = "{\"src\":1,\"alias\":\"Z\",\"eot\":true}\n";
    const std::string b = "{\"src\":2,\"eot\":false}\n", bEnd = "{\"src\":2,\"eot\":true}\n";
    {
        CallLog log;
        std::string s;
        for (int i = 0; i < 3; i++) s += see(log, {{"a", members(1), false}});
        check(s, a, "a call seen in three polls with the same members gives one line");
        check(see(log, {{"a", members(1, "Z"), false}}), aAlias, "a member that changes gives the call's members again");
        check(see(log, {{"a", members(1, "Z"), true}}), aAliasEnd, "a call marked ended gives its last members with eot true");
        s.clear();
        for (int i = 0; i < 3; i++) s += see(log, {{"a", members(1, "Z"), true}});
        check(s, "", "an ended call still in the log gives nothing more");
        check(see(log, {{"a", members(1), false}}), a, "the same key open again after its end is a new call: a new first line");
        s = see(log, {});
        s += see(log, {});
        check(s, aEnd, "a call that leaves the log without being marked ended gives one last line");
        check(see(log, {{"a", members(1), false}}), a, "the same key back after it left the log is a new call");
    }
    {
        CallLog log;
        see(log, {{"a", members(1), true}});
        std::string s = see(log, {});   // in turn: the operands of + are not sequenced
        s += see(log, {{"b", members(2), false}});
        check(s, b, "an ended call that ages out of the log gives nothing");
        check(see(log, {{"b", members(2), false}, {"c", members(3), true}}),
              "{\"src\":3,\"eot\":false}\n{\"src\":3,\"eot\":true}\n", "a call first seen ended gives its first and its last line at once");
        check(finish(log), bEnd, "finish ends the call still open");
        check(finish(log), "", "finish ends it once");
    }
    {
        CallLog log;
        check(see(log, {{"a", members(1), false}, {"b", members(2), false}}), a + b, "two calls at once: a first line each");
        check(see(log, {{"b", members(2), false}}), aEnd, "one of them leaving the log ends that one only");
        check(see(log, {{"b", members(2), true}, {"a", members(1), false}}), bEnd + a, "lines in the snapshot's order");
    }
    return fails ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--selftest") {
        if (std::string(argv[2]) == "ais-join") return selftestAisJoin();
        if (std::string(argv[2]) == "sonde-id") return selftestSondeId();
        if (std::string(argv[2]) == "json-utf8") return selftestJsonUtf8();
        if (std::string(argv[2]) == "calls") return selftestCalls();
        return usage();
    }
    std::string mode, text = "HELLO ZETA", info, ps = "ZETA FM ", rt = "HELLO FROM ZETASDR";
    std::string service = "ZETA TEST";
    bool haveText = false, haveInfo = false, haveStation = false, channel = false, haveService = false;
    std::string sigmf;   // --sigmf STEM: the SigMF pair instead of stdout
    double rate = 0, freq = 0, deviation = 0, lead = 0;
    int bandwidth = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool val = i + 1 < argc;
        if (a == "--mode" && val) mode = argv[++i];
        else if (a == "--channel") channel = true;
        else if (a == "--rate" && val) rate = atof(argv[++i]);
        else if (a == "--freq" && val) { freq = atof(argv[++i]); if (freq <= 0) return usage(); }
        else if (a == "--text" && val) { text = argv[++i]; haveText = true; }
        else if (a == "--info" && val) { info = argv[++i]; haveInfo = true; }
        else if (a == "--ps" && val) { ps = argv[++i]; haveStation = true; }
        else if (a == "--rt" && val) { rt = argv[++i]; haveStation = true; }
        else if (a == "--bandwidth" && val) { bandwidth = atoi(argv[++i]); if (bandwidth < 5 || bandwidth > 8) return usage(); }
        else if (a == "--service" && val) { service = argv[++i]; haveService = true; }
        else if (a == "--lead" && val) { lead = atof(argv[++i]); if (lead <= 0) return usage(); }
        else if (a == "--deviation" && val) { deviation = atof(argv[++i]); if (deviation <= 0) return usage(); }
        else if (a == "--sigmf" && val) { sigmf = argv[++i]; if (sigmf.empty()) return usage(); }
        else return usage();
    }
    if (rate <= 0) return usage();
    // each option belongs to its modes: one another mode would ignore is refused
    const bool voice = mode == "dmr" || mode == "p25" || mode == "m17";
    const bool tv = mode == "dvbt" || mode == "video";
    const bool scene = mode == "ble" || mode == "lorawan" || mode == "zigbee" || mode == "thread" || mode == "zwave" || mode == "wifi" ||
                       mode == "wisun";   // a scene of OnAir's own: only --freq
    if (mode == "rds" ? freq || haveText || haveInfo : voice || tv ? freq || haveText || haveInfo || haveStation : haveStation) return usage();
    if (scene && (haveText || haveInfo)) return usage();
    if (channel && mode != "dmr" && mode != "p25") return usage();
    if ((bandwidth || haveService || lead) && mode != "dvbt") return usage();
    if (deviation && mode != "video") return usage();
    // the centre the pair records: --freq, or the mode's own (ACARS's channel, the middle of AIS 1 and 2, Bluetooth LE's channel
    // 37, the LoRaWAN scene's centre); 0 for a mode with neither. ACARS's default is also every other mode's freq, which they ignore
    const bool ownCentre = mode == "acars" || mode == "ais" || scene;
    const double centre = freq ? freq : !ownCentre ? 0 : mode == "ais" ? 162e6 : mode == "ble" ? 2402e6
                                      : mode == "lorawan" ? lorawanPlanCentreMhz(lorawan::EU868) * 1e6
                                      : mode == "zigbee" || mode == "thread" ? 2425e6   // channel 15
                                      : mode == "zwave" ? zwave::region(kZwaveRegion)->centreMhz * 1e6
                                      : mode == "wifi" ? 2437e6                         // channel 6
                                      : mode == "wisun" ? wisunTuning().defMhz * 1e6     // the scene's own centre
                                      : 131.525e6;
    if (!freq) freq = centre ? centre : 131.525e6;
    if (mode == "rds" || tv) {   // a broadcast is written as it is made: no cut and no silence (rds's and video's noise are the generator's own)
        const std::vector<cf32> x = mode == "rds" ? rdsSignal(rate, ps, rt) : mode == "dvbt" ? dvbtSignal(rate, bandwidth ? bandwidth : 8, service, lead)
                                                                             : videoSignal(rate, deviation);
        if (x.empty()) { fprintf(stderr, "onair-gen: no %s signal at %.0f Hz\n", mode.c_str(), rate); return 1; }
        if (!writeOut(x, sigmf, rate, centre, {{0, x.size()}})) return 1;
        fprintf(stderr, "onair-gen: %s message from 0.000 s to %.3f s, %zu samples at %.0f Hz\n", mode.c_str(),
                (double)x.size() / rate, x.size(), rate);
        return 0;
    }
    std::vector<cf32> msg;
    std::vector<WisunSpan> wisunFrames;   // wisun: where each frame of the scene is in msg
    if (mode == "pocsag" || mode == "flex") msg = pagerSignal(rate, mode == "flex", text);
    else if (mode == "acars") msg = acarsSignal(rate, freq, text);
    else if (mode == "aprs") msg = aprsSignal(rate, haveInfo ? info : "!2723.05S/15307.05E>" + (haveText ? text : std::string("ZetaSDR")));
    else if (mode == "ais") msg = aisSignal(rate, freq);
    else if (mode == "vdl2") msg = vdl2Signal(rate, text);   // VDL2's, HFDL's and a sonde's signal is the same at any --freq
    else if (mode == "hfdl") msg = hfdlSignal(rate);
    else if (mode == "rs41" || mode == "dfm" || mode == "m20") msg = sondeSignal(rate, mode);
    else if (mode == "dmr") msg = dmrSignal(rate, channel);
    else if (mode == "p25") msg = p25Signal(rate, channel);
    else if (mode == "m17") msg = m17Signal(rate);
    else if (mode == "ble") msg = bleSignal(rate);
    else if (mode == "lorawan") msg = lorawanSignal(rate, freq);
    else if (mode == "zigbee") msg = zigbeeSignal(rate);
    else if (mode == "thread") msg = threadSignal(rate);
    else if (mode == "zwave") msg = zwaveSignal(rate, freq);
    else if (mode == "wifi") msg = wifiSignal(rate);
    else if (mode == "wisun") msg = wisunSignal(rate, freq, &wisunFrames);
    else return usage();
    if (msg.empty()) { fprintf(stderr, "onair-gen: no %s signal at %.0f Hz\n", mode.c_str(), rate); return 1; }

    double power = 0;
    size_t on = 0;
    for (const cf32& v : msg)
        if (std::norm(v) > 1e-12) { power += std::norm(v); on++; }
    const double carrier = std::sqrt(power / (double)on);
    const size_t pad = (size_t)std::llround((mode == "wifi" ? kWifiPadSec : kPadSec) * rate);
    const size_t again = voice ? pad + msg.size() + (size_t)std::llround(kRepeatGapSec * rate) : 0;   // where the second call starts
    std::vector<cf32> out((voice ? again : pad) + msg.size() + pad, cf32(0.f, 0.f));
    std::copy(msg.begin(), msg.end(), out.begin() + (long)pad);
    if (voice) std::copy(msg.begin(), msg.end(), out.begin() + (long)again);
    // kSnrDb in the mode's channel: 25 kHz, Bluetooth LE's 1 MHz, a LoRaWAN channel's 125 kHz
    const double noiseBw = mode == "ble" ? 1e6 : mode == "lorawan" ? 125e3 : mode == "zigbee" || mode == "thread" ? 2e6
                         : mode == "zwave" ? 100e3 : mode == "wifi" ? 20e6 : mode == "wisun" ? 400e3 : 25000.0;
    genutil::NoiseSource noise(7);
    noise.add(out.data(), out.size(), (float)(carrier * std::sqrt(rate / (2 * noiseBw * std::pow(10.0, kSnrDb / 10)))));
    std::vector<Span> spans = {{pad, msg.size()}};
    if (voice) spans.push_back({again, msg.size()});
    if (!writeOut(out, sigmf, rate, centre, spans)) return 1;
    char twice[96] = "";
    if (voice) snprintf(twice, sizeof twice, " and again from %.3f s to %.3f s", (double)again / rate, (double)(again + msg.size()) / rate);
    fprintf(stderr, "onair-gen: %s message from %.3f s to %.3f s%s, %zu samples at %.0f Hz\n", mode.c_str(), (double)pad / rate,
            (double)(pad + msg.size()) / rate, twice, out.size(), rate);
    for (size_t i = 0; i < wisunFrames.size(); i++) {
        const WisunSpan& w = wisunFrames[i];
        fprintf(stderr, "onair-gen: frame %zu from %.6f s to %.6f s, %d bit/s on %.0f Hz\n", i + 1, (double)(pad + w.start) / rate,
                (double)(pad + w.start + w.count) / rate, w.bitRate, w.hz);
    }
    return 0;
}
