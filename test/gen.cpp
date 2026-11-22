// SPDX-License-Identifier: GPL-3.0-or-later
// onair-gen: one known message of a mode from OnAir's own test-signal generator, as cf32 IQ on stdout, with a second of noise
// either side. For onair-rx's tests (test/bus.sh); nothing is transmitted.
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
//   onair-gen --mode pocsag|flex|acars|aprs|ais --rate HZ [--freq HZ] [--text TEXT]
//   onair-gen --selftest ais-join     onair-rx's AIS sentence joiner (src/ais.cpp) on sentences made here; exit 1 on a failure
//
// The known messages: POCSAG address 1234567 function 3 "HELLO ZETA" at 1200 baud (alphanumeric); FLEX capcode 1234567
// "HELLO ZETA" (alphanumeric, 1600/2); ACARS tail VH-ZET label H1 flight ZET001 ("HELLO ZETA" after the message number and
// flight); APRS VK3ABC>APRS:!2723.05S/15307.05E>ZetaSDR. --text replaces the text (APRS: the comment after the position).
// AIS: two messages of MMSI 503000001, a position report (type 1: -27.3842, 153.1175, 12.3 kt, course 45.0 and heading 45
// degrees, under way using engine) on AIS 1, then a static report (type 5, two sentences) named ZETA TEST on AIS 2.
// The channel is at 0 Hz of the output, as onair-rx expects it: the pager and packet generators put it at their mode's tune
// offset, which is cancelled here; ACARS is generated for one channel at --freq (default 131.525 MHz) with the centre there;
// AIS 1 and 2 are placed by their frequencies (161.975 and 162.025 MHz) around --freq (default 162 MHz, between the two).
//
// OnAir's generator makes the message without noise (ACARS: with its own noise 60 dB down); it is cut from its first to its
// last sample (ACARS: the burst the generator logs), a second of silence is put either side, and noise is added over the whole
// at 25 dB carrier to noise in 25 kHz (the carrier: the mean power of the samples that are not zero, so AIS's gap between its
// bursts does not count). stderr says where the message is: "message from A s to B s".
#include "ais.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
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
constexpr double kSnrDb = 25;   // carrier to noise in 25 kHz
constexpr size_t kChunk = 1 << 14;

int usage() {
    fprintf(stderr, "usage: onair-gen --mode pocsag|flex|acars|aprs|ais --rate HZ [--freq HZ] [--text TEXT] > iq\n"
                    "       onair-gen --selftest ais-join\n");
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

std::vector<cf32> aprsSignal(double rate, const std::string& comment) {
    ax25::Frame f;
    f.to.call = "APRS";
    f.from.call = "VK3ABC";
    f.info = "!2723.05S/15307.05E>" + comment;
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

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--selftest") {
        if (std::string(argv[2]) == "ais-join") return selftestAisJoin();
        return usage();
    }
    std::string mode, text = "HELLO ZETA";
    bool haveText = false;
    double rate = 0, freq = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool val = i + 1 < argc;
        if (a == "--mode" && val) mode = argv[++i];
        else if (a == "--rate" && val) rate = atof(argv[++i]);
        else if (a == "--freq" && val) { freq = atof(argv[++i]); if (freq <= 0) return usage(); }
        else if (a == "--text" && val) { text = argv[++i]; haveText = true; }
        else return usage();
    }
    if (!freq) freq = mode == "ais" ? 162e6 : 131.525e6;
    if (rate <= 0) return usage();
    std::vector<cf32> msg;
    if (mode == "pocsag" || mode == "flex") msg = pagerSignal(rate, mode == "flex", text);
    else if (mode == "acars") msg = acarsSignal(rate, freq, text);
    else if (mode == "aprs") msg = aprsSignal(rate, haveText ? text : "ZetaSDR");
    else if (mode == "ais") msg = aisSignal(rate, freq);
    else return usage();
    if (msg.empty()) { fprintf(stderr, "onair-gen: no %s signal at %.0f Hz\n", mode.c_str(), rate); return 1; }

    double power = 0;
    size_t on = 0;
    for (const cf32& v : msg)
        if (v != cf32(0.f, 0.f)) { power += std::norm(v); on++; }
    const double carrier = std::sqrt(power / (double)on);
    const size_t pad = (size_t)std::llround(kPadSec * rate);
    std::vector<cf32> out(pad + msg.size() + pad, cf32(0.f, 0.f));
    std::copy(msg.begin(), msg.end(), out.begin() + (long)pad);
    genutil::NoiseSource noise(7);
    noise.add(out.data(), out.size(), (float)(carrier * std::sqrt(rate / (2 * 25000.0 * std::pow(10.0, kSnrDb / 10)))));
    if (fwrite(out.data(), sizeof(cf32), out.size(), stdout) != out.size() || fflush(stdout) != 0) {
        fprintf(stderr, "onair-gen: stdout: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "onair-gen: %s message from %.3f s to %.3f s, %zu samples at %.0f Hz\n", mode.c_str(), (double)pad / rate,
            (double)(pad + msg.size()) / rate, out.size(), rate);
    return 0;
}
