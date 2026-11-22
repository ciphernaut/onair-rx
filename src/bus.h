// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the bus modes. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// A bus mode is one OnAir receiver whose decodes are taken from OnAir's automation bus and whose frames are taken from OnAir's
// Wireshark hub, both on this thread inside the receiver's feed(). One generic run serves every row of the table.
#pragma once
#include "rx.h"
#include "dect2/automation/event.h"
#include <functional>
#include <string>

namespace onair_rx {

struct BusMode {
    const char* name;                 // "pocsag"
    int chan;                         // a dect2::wire::Chan, or -1: no packets
    bool needsFreq;                   // the receiver places its channels by frequency: --freq is required
    // makes the receiver, sets it up for o, and returns its feed; false with *err set when o does not suit the mode
    bool (*open)(const Options& o, std::function<void(const cf32*, size_t)>* feed, std::string* err);
    // adds the members the row needs and the bus lacks; false drops the event
    bool (*shape)(const dect2::automation::Event& e, Json& j);
    // a mode whose lines come from its hub packets (the hub then runs with or without a pcap port): the lines' event, and what
    // makes, once per run, the function that adds a packet's members to its line (false: no line for this packet). nullptr: none
    const char* packetEvent;
    std::function<bool(const uint8_t* p, size_t n, Json& j)> (*packetLines)();
};

extern const BusMode kBusModes[];
extern const size_t kBusModeCount;

// Runs a bus mode to EOF: one JSON line per bus event (and per packet packetLines takes), one pcap record per hub packet (with a
// pcap port), wrapped as OnAir's capture files wrap the channel's packets. The exit status.
int runBus(const BusMode& m, const Options& o, IqInput& in, Output& out);

} // namespace onair_rx
