// onair-rx spike: IQ on stdin -> one OnAir receiver -> OnAir's Wireshark hub -> classic pcap on stdout.
// SPDX-License-Identifier: GPL-3.0-or-later
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// Spike only: ADS-B through AdsbReceiver's configure()/feed() contract. The hub's own live stream (the extcap
// protocol, 127.0.0.1, any free port) is read back in this process and copied to stdout with a pcap file header,
// so OnAir is used unchanged.
#include "dect2/adsb/adsb_rx.h"
#include "dect2/wirecap/wire.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;
namespace wire = dect2::wire;

static void usage() {
    fprintf(stderr, "usage: onair-rx --mode adsb --rate HZ [--format cf32|cs16] < iq > out.pcap\n");
}

int main(int argc, char** argv) {
    std::string mode = "adsb", format = "cf32";
    double rate = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) format = argv[++i];
        else if (a == "--lua") { fputs(wire::luaDissectors(), stdout); return 0; }
        else { usage(); return 2; }
    }
    if (mode != "adsb" || rate <= 0 || (format != "cf32" && format != "cs16")) { usage(); return 2; }

    wire::Config cfg;
    cfg.dir = ""; cfg.files = false; cfg.live = true; cfg.gsmtap = false; cfg.streamPort = 0;
    auto& hub = wire::Hub::get();
    hub.configure(cfg);
    hub.setEnabled(wire::AdsB, true);
    int port = 0;
    for (int i = 0; i < 200 && (port = hub.boundPort()) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!port) { fprintf(stderr, "onair-rx: the hub did not open its live port\n"); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { perror("onair-rx: connect"); return 1; }
    const std::string hello = "ONAIR-WIRE 1 " + mode + "\n";
    if (write(s, hello.data(), hello.size()) != (ssize_t)hello.size()) { perror("onair-rx: hello"); return 1; }
    std::string line; char c;
    while (read(s, &c, 1) == 1 && c != '\n') line += c;
    if (line.rfind("OK ", 0) != 0) { fprintf(stderr, "onair-rx: hub answered '%s'\n", line.c_str()); return 1; }
    const uint32_t linktype = (uint32_t)atoi(line.c_str() + 3);
    const wire::Bytes head = wire::pcapFileHeader(linktype);
    fwrite(head.data(), 1, head.size(), stdout);
    fflush(stdout);
    for (int i = 0; i < 200 && hub.stats(wire::AdsB).clients == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread pump([s] {
        char buf[65536];
        for (;;) {
            const ssize_t n = read(s, buf, sizeof buf);
            if (n <= 0) break;
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    });

    AdsbReceiver rx;
    std::atomic<uint64_t> frames{0};
    rx.setFrameCallback([&](const AdsbFrame&) { frames++; });
    rx.configure(rate);
    if (!rx.ready()) { fprintf(stderr, "onair-rx: %g Hz is too low for %s\n", rate, mode.c_str()); return 1; }

    const size_t block = 1 << 16;
    std::vector<cf32> iq(block);
    std::vector<int16_t> raw(2 * block);
    uint64_t total = 0;
    for (;;) {
        size_t n;
        if (format == "cf32") n = fread(iq.data(), sizeof(cf32), block, stdin);
        else {
            n = fread(raw.data(), 2 * sizeof(int16_t), block, stdin);
            for (size_t i = 0; i < n; i++) iq[i] = cf32(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
        }
        if (n == 0) break;
        rx.feed(iq.data(), n);
        total += n;
    }
    hub.waitIdle(3000);
    const wire::Stats st = hub.stats(wire::AdsB);
    hub.shutdown();
    pump.join();
    close(s);
    fprintf(stderr, "onair-rx: %llu samples (%.2f s), %llu frames from the receiver, hub %llu packets, %llu dropped\n",
            (unsigned long long)total, total / rate, (unsigned long long)frames.load(),
            (unsigned long long)st.packets, (unsigned long long)st.dropped);
    return 0;
}
