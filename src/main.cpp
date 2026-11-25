// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: one OnAir receiver on IQ from stdin, its decodes as JSON lines on stdout and its frames as pcap on a local port,
// so that ZetaSDR (or anything else) can run OnAir as an external decoder.
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// Copyright (C) 2026 the onair-rx authors
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// It is distributed WITHOUT ANY WARRANTY; see the LICENSE file.
#include "rx.h"
#include "bus.h"
#include "poll.h"
#include "dect2/wirecap/wire.h"
#include <cerrno>
#include <cmath>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "unknown"
#endif

namespace onair_rx {

namespace wire = dect2::wire;

static const char* kVersion = "0.4.0";

// The modes with a run function of their own: each is one OnAir receiver. The modes read from OnAir's automation bus are
// rows of the bus mode table (bus.cpp) instead, and the modes that poll their receiver's state rows of the polled mode table
// (poll.cpp).
static const Mode kModes[] = {
    {"adsb", wire::kChans[wire::AdsB].linktype, runAdsb},
};

// The options only some modes take, and the modes that take each (space separated). A mode not named refuses the option with
// "onair-rx: <mode> takes no --<option>", before reading any input.
struct OnlySome {
    const char* option;
    const char* modes;
    bool (*given)(const Options& o);
};
static const OnlySome kOnlySome[] = {
    {"width", "dvbs", [](const Options& o) { return o.width > 0; }},
    {"plan", "lorawan", [](const Options& o) { return !o.plan.empty(); }},
    {"region", "zwave wisun", [](const Options& o) { return !o.region.empty(); }},
    {"drop", "wisun tetra gsmr lte dvbs", [](const Options& o) { return o.drop; }},
};
static bool takes(const char* modes, const std::string& mode) {
    const std::string list = std::string(" ") + modes + " ";
    return list.find(" " + mode + " ") != std::string::npos;
}

size_t IqInput::read(std::vector<cf32>& out) {
    const size_t per = cs16_ ? 4 : 8;
    buf_.resize(out.size() * per);
    while (have_ < per) {
        const ssize_t k = ::read(0, buf_.data() + have_, buf_.size() - have_);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) error_ = errno;
        if (k <= 0) return 0;   // EOF (a cut sample is dropped) or an error (error())
        have_ += (size_t)k;
    }
    if (!startUsec_)
        startUsec_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const size_t n = have_ / per;
    if (cs16_) {
        for (size_t i = 0; i < n; i++) {
            int16_t v[2];
            memcpy(v, buf_.data() + i * 4, 4);
            out[i] = cf32(v[0] / 32768.0f, v[1] / 32768.0f);
        }
    } else {
        memcpy(out.data(), buf_.data(), n * 8);
    }
    have_ -= n * per;
    memmove(buf_.data(), buf_.data() + n * per, have_);
    samples_ += n;
    return n;
}

void Output::line(const std::string& json) {
    fputs(json.c_str(), stdout);
    fputc('\n', stdout);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "onair-rx: stdout: %s\n", strerror(errno));
        _exit(1);
    }
}

} // namespace onair_rx

using namespace onair_rx;

static void usage(FILE* f) {
    fprintf(f, "usage: onair-rx --mode MODE --rate HZ [--format cf32|cs16] [--freq HZ] [--pcap-port PORT] [--bandwidth MHZ]\n"
               "                [--width HZ] [--plan NAME] [--region NAME] [--drop] < iq\n"
               "       onair-rx --lua      OnAir's Wireshark Lua dissector (USER0 is Mode S)\n"
               "       onair-rx --version\n"
               "IQ on stdin, interleaved little endian; --freq is the radio frequency at 0 Hz of the input; one JSON object\n"
               "per decode on stdout; with --pcap-port, a classic pcap stream of the frames for one client at a time on\n"
               "127.0.0.1:PORT; --bandwidth (dvbt: 8, 7, 6 or 5) fixes the channel width, which is otherwise found; --plan\n"
               "(lorawan) names the channel plan, which is otherwise the one with most channels in the input; --region (zwave)\n"
               "the Z-Wave region, which is otherwise the one with most channels in the input, and (wisun) the Wi-SUN band\n"
               "plan, which is otherwise the one for --freq; --drop (wisun, tetra, gsmr, lte, dvbs) lets the receiver drop\n"
               "what its decoder has no time for (wisun: bursts; tetra, gsmr, lte: the input while a stretch is decoded; dvbs:\n"
               "frames), as it does live, instead of waiting for it; --width (dvbs) is the band the carrier is in, around\n"
               "0 Hz. Modes:");
    for (const Mode& m : kModes) fprintf(f, " %s", m.name);
    for (size_t i = 0; i < kBusModeCount; i++) fprintf(f, " %s", kBusModes[i].name);
    for (size_t i = 0; i < kPollModeCount; i++) fprintf(f, " %s", kPollModes[i].name);
    fprintf(f, "\n");
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    Options o;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool val = i + 1 < argc;
        if (a == "--mode" && val) o.mode = argv[++i];
        else if (a == "--rate" && val) o.rate = atof(argv[++i]);
        else if (a == "--format" && val) o.format = argv[++i];
        else if (a == "--freq" && val) {   // every mode takes it; only the modes that place channels by frequency read it
            char* end = nullptr;
            const double hz = strtod(argv[++i], &end);
            if (!*argv[i] || *end || !std::isfinite(hz) || hz <= 0) { fprintf(stderr, "onair-rx: bad --freq %s\n", argv[i]); return 2; }
            o.freq = hz;
        }
        else if (a == "--pcap-port" && val) {
            char* end = nullptr;
            const long p = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || p < 1 || p > 65535) { fprintf(stderr, "onair-rx: bad --pcap-port %s\n", argv[i]); return 2; }
            o.pcapPort = (int)p;
        }
        else if (a == "--ts-out" && val) o.tsOut = argv[++i];   // spike: dvbt's transport stream, as decoded
        else if (a == "--bandwidth" && val) {
            char* end = nullptr;
            const long w = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || w < 5 || w > 8) { fprintf(stderr, "onair-rx: bad --bandwidth %s (8, 7, 6 or 5)\n", argv[i]); return 2; }
            o.bandwidthMhz = (int)w;
        }
        else if (a == "--width" && val) {
            char* end = nullptr;
            const double hz = strtod(argv[++i], &end);
            if (!*argv[i] || *end || !std::isfinite(hz) || hz <= 0) { fprintf(stderr, "onair-rx: bad --width %s\n", argv[i]); return 2; }
            o.width = hz;
        }
        else if (a == "--plan" && val) {
            o.plan = argv[++i];
            if (o.plan.empty()) { fprintf(stderr, "onair-rx: bad --plan (empty)\n"); return 2; }
        }
        else if (a == "--region" && val) {
            o.region = argv[++i];
            if (o.region.empty()) { fprintf(stderr, "onair-rx: bad --region (empty)\n"); return 2; }
        }
        else if (a == "--drop") o.drop = true;
        else if (a == "--lua") { fputs(wire::luaDissectors(), stdout); return 0; }
        else if (a == "--version") {
            printf("onair-rx %s\nOnAir %s\nLicence: GPL-3.0-or-later\n"
                   "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev\n"
                   "OnAir contributors: https://github.com/adswill/OnAir/blob/main/CONTRIBUTORS.md\n",
                   kVersion, ONAIR_VERSION);
            return 0;
        }
        else if (a == "--help" || a == "-h") { usage(stdout); return 0; }
        else { usage(stderr); return 2; }
    }
    const Mode* mode = nullptr;
    const BusMode* bus = nullptr;
    const PollMode* polled = nullptr;
    for (const Mode& m : kModes) if (o.mode == m.name) mode = &m;
    for (size_t i = 0; i < kBusModeCount; i++) if (o.mode == kBusModes[i].name) bus = &kBusModes[i];
    for (size_t i = 0; i < kPollModeCount; i++) if (o.mode == kPollModes[i].name) polled = &kPollModes[i];
    if (!mode && !bus && !polled) { fprintf(stderr, "onair-rx: no mode %s\n", o.mode.c_str()); return 2; }
    for (const OnlySome& s : kOnlySome)
        if (s.given(o) && !takes(s.modes, o.mode)) { fprintf(stderr, "onair-rx: %s takes no --%s\n", o.mode.c_str(), s.option); return 2; }
    if (o.rate <= 0) { fprintf(stderr, "onair-rx: --rate HZ is needed\n"); return 2; }
    if (o.format != "cf32" && o.format != "cs16") { fprintf(stderr, "onair-rx: no format %s (cf32 or cs16)\n", o.format.c_str()); return 2; }
    // before any sample is read: a mode that places its channels by frequency never runs without one
    if (bus && bus->needsFreq && o.freq <= 0) { fprintf(stderr, "onair-rx: %s needs --freq HZ\n", bus->name); return 2; }
    if (polled && polled->needsFreq && o.freq <= 0) { fprintf(stderr, "onair-rx: %s needs --freq HZ\n", polled->name); return 2; }
    if (bus && bus->chan < 0 && o.pcapPort) { fprintf(stderr, "onair-rx: %s has no packets (no --pcap-port)\n", bus->name); return 2; }
    if (!o.tsOut.empty() && o.mode != "dvbt") { fprintf(stderr, "onair-rx: %s has no --ts-out (dvbt only)\n", o.mode.c_str()); return 2; }
    if (o.bandwidthMhz && o.mode != "dvbt") { fprintf(stderr, "onair-rx: %s has no --bandwidth (dvbt only)\n", o.mode.c_str()); return 2; }
    if (polled && polled->chan < 0 && o.pcapPort) { fprintf(stderr, "onair-rx: %s has no packets (no --pcap-port)\n", polled->name); return 2; }

    PcapPort pcap;
    if (o.pcapPort) {
        std::string err;
        const uint32_t linktype = bus ? wire::kChans[bus->chan].linktype : polled ? wire::kChans[polled->chan].linktype : mode->linktype;
        if (!pcap.listen(o.pcapPort, linktype, err)) {
            fprintf(stderr, "onair-rx: cannot listen on 127.0.0.1:%d: %s\n", o.pcapPort, err.c_str());
            return 1;
        }
    }
    IqInput in(o.format);
    Output out(o.pcapPort ? &pcap : nullptr);
    const int st = bus ? runBus(*bus, o, in, out) : polled ? runPolled(*polled, o, in, out) : mode->run(o, in, out);
    fflush(stdout);
    pcap.finish();
    if (in.error()) { fprintf(stderr, "onair-rx: stdin: %s\n", strerror(in.error())); return 1; }
    if (st != 0) return st;
    fprintf(stderr, "onair-rx: %llu samples (%.2f s)", (unsigned long long)in.samples(), in.samples() / o.rate);
    if (o.pcapPort) fprintf(stderr, ", pcap %llu sent, %llu dropped", (unsigned long long)pcap.sent(), (unsigned long long)pcap.dropped());
    fprintf(stderr, "\n");
    return st;
}
