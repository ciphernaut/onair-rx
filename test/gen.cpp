// SPDX-License-Identifier: GPL-3.0-or-later
// onair-gen: one known message of a mode from OnAir's own test-signal generator, as cf32 IQ on stdout, with a second of noise
// either side. For onair-rx's tests (test/bus.sh); nothing is transmitted.
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
//   onair-gen --mode pocsag|flex|acars|aprs --rate HZ [--freq HZ] [--text TEXT]
//
// The known messages: POCSAG address 1234567 function 3 "HELLO ZETA" at 1200 baud (alphanumeric); FLEX capcode 1234567
// "HELLO ZETA" (alphanumeric, 1600/2); ACARS tail VH-ZET label H1 flight ZET001 ("HELLO ZETA" after the message number and
// flight); APRS VK3ABC>APRS:!2723.05S/15307.05E>ZetaSDR. --text replaces the text (APRS: the comment after the position).
// The channel is at 0 Hz of the output, as onair-rx expects it: the pager and packet generators put it at their mode's tune
// offset, which is cancelled here; ACARS is generated for one channel at --freq (default 131.525 MHz) with the centre there.
//
// OnAir's generator makes the message without noise (ACARS: with its own noise 60 dB down); it is cut from its first to its
// last sample (ACARS: the burst the generator logs), a second of silence is put either side, and noise is added over the whole
// at 25 dB carrier to noise in 25 kHz. stderr says where the message is: "message from A s to B s".
#include "dect2/acars/acars_gen.h"
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
    fprintf(stderr, "usage: onair-gen --mode pocsag|flex|acars|aprs --rate HZ [--freq HZ] [--text TEXT] > iq\n");
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

} // namespace

int main(int argc, char** argv) {
    std::string mode, text = "HELLO ZETA";
    bool haveText = false;
    double rate = 0, freq = 131.525e6;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool val = i + 1 < argc;
        if (a == "--mode" && val) mode = argv[++i];
        else if (a == "--rate" && val) rate = atof(argv[++i]);
        else if (a == "--freq" && val) freq = atof(argv[++i]);
        else if (a == "--text" && val) { text = argv[++i]; haveText = true; }
        else return usage();
    }
    if (rate <= 0 || freq <= 0) return usage();
    std::vector<cf32> msg;
    if (mode == "pocsag" || mode == "flex") msg = pagerSignal(rate, mode == "flex", text);
    else if (mode == "acars") msg = acarsSignal(rate, freq, text);
    else if (mode == "aprs") msg = aprsSignal(rate, haveText ? text : "ZetaSDR");
    else return usage();
    if (msg.empty()) { fprintf(stderr, "onair-gen: no %s signal at %.0f Hz\n", mode.c_str(), rate); return 1; }

    double power = 0;
    for (const cf32& v : msg) power += std::norm(v);
    const double carrier = std::sqrt(power / (double)msg.size());
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
