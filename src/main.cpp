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
#include "dect2/wirecap/wire.h"
#include <cerrno>
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

static const char* kVersion = "0.1.0";

// The modes: each is one OnAir receiver. Another mode is one more row and its run function.
static const Mode kModes[] = {
    {"adsb", wire::kChans[wire::AdsB].linktype, runAdsb},
};

size_t IqInput::read(std::vector<cf32>& out) {
    const size_t per = cs16_ ? 4 : 8;
    buf_.resize(out.size() * per);
    while (have_ < per) {
        const ssize_t k = ::read(0, buf_.data() + have_, buf_.size() - have_);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;   // EOF (a cut sample is dropped) or an error
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

namespace {
void escape(std::string& o, const std::string& v) {
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20 || c >= 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
}
} // namespace

void Json::key(const char* k) {
    if (s_.size() > 1) s_ += ',';
    s_ += '"'; s_ += k; s_ += "\":";
}
Json& Json::str(const char* k, const std::string& v) { key(k); s_ += '"'; escape(s_, v); s_ += '"'; return *this; }
Json& Json::num(const char* k, long long v) { key(k); s_ += std::to_string(v); return *this; }
Json& Json::num(const char* k, double v, int decimals) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", decimals, v);
    key(k); s_ += b; return *this;
}
Json& Json::flag(const char* k, bool v) { key(k); s_ += v ? "true" : "false"; return *this; }

} // namespace onair_rx

using namespace onair_rx;

static void usage(FILE* f) {
    fprintf(f, "usage: onair-rx --mode MODE --rate HZ [--format cf32|cs16] [--pcap-port PORT] < iq\n"
               "       onair-rx --lua      OnAir's Wireshark Lua dissector (USER0 is Mode S)\n"
               "       onair-rx --version\n"
               "IQ on stdin, interleaved little endian; one JSON object per decode on stdout; with --pcap-port, a classic\n"
               "pcap stream of the frames for one client at a time on 127.0.0.1:PORT. Modes:");
    for (const Mode& m : kModes) fprintf(f, " %s", m.name);
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
        else if (a == "--pcap-port" && val) {
            char* end = nullptr;
            const long p = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || p < 1 || p > 65535) { fprintf(stderr, "onair-rx: bad --pcap-port %s\n", argv[i]); return 2; }
            o.pcapPort = (int)p;
        }
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
    for (const Mode& m : kModes) if (o.mode == m.name) mode = &m;
    if (!mode) { fprintf(stderr, "onair-rx: no mode %s\n", o.mode.c_str()); return 2; }
    if (o.rate <= 0) { fprintf(stderr, "onair-rx: --rate HZ is needed\n"); return 2; }
    if (o.format != "cf32" && o.format != "cs16") { fprintf(stderr, "onair-rx: no format %s (cf32 or cs16)\n", o.format.c_str()); return 2; }

    PcapPort pcap;
    if (o.pcapPort) {
        std::string err;
        if (!pcap.listen(o.pcapPort, mode->linktype, err)) {
            fprintf(stderr, "onair-rx: cannot listen on 127.0.0.1:%d: %s\n", o.pcapPort, err.c_str());
            return 1;
        }
    }
    IqInput in(o.format);
    Output out(o.pcapPort ? &pcap : nullptr);
    const int st = mode->run(o, in, out);
    fflush(stdout);
    pcap.finish();
    if (st != 0) return st;
    fprintf(stderr, "onair-rx: %llu samples (%.2f s)", (unsigned long long)in.samples(), in.samples() / o.rate);
    if (o.pcapPort) fprintf(stderr, ", pcap %llu sent, %llu dropped", (unsigned long long)pcap.sent(), (unsigned long long)pcap.dropped());
    fprintf(stderr, "\n");
    return st;
}
