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
    // the row's event: the one bus event written as a line (every other event the receiver puts on the bus is dropped), and
    // the event of the lines made from hub packets
    const char* event;
    int chan;                         // a dect2::wire::Chan, or -1: no packets
    bool needsFreq;                   // the receiver places its channels by frequency: --freq is required
    // makes the receiver, sets it up for o, and returns its feed; false with *err set when o does not suit the mode
    bool (*open)(const Options& o, std::function<void(const cf32*, size_t)>* feed, std::string* err);
    // adds the members the row needs and the bus lacks to a line of the row's event; false drops the event. nullptr: none
    bool (*shape)(const dect2::automation::Event& e, Json& j);
    // a mode whose lines come from its hub packets (the hub then runs with or without a pcap port): what makes, once per run,
    // the function that adds a packet's members to its line (false: no line for this packet). nullptr: none
    std::function<bool(const uint8_t* p, size_t n, Json& j)> (*packetLines)();
};

extern const BusMode kBusModes[];
extern const size_t kBusModeCount;

// Runs a bus mode to EOF: one JSON line per bus event of the mode's event that shape keeps (and per packet packetLines takes),
// one pcap record per hub packet (with a pcap port), wrapped as OnAir's capture files wrap the channel's packets. The exit
// status.
int runBus(const BusMode& m, const Options& o, IqInput& in, Output& out);

// A sonde's id: OnAir's serial (sonde.update's serial, of type "RS41", "DFM" or "M20") spelled as rs1729's decoders write their
// "id", on which ZetaSDR's sonde rows key their subjects (here, so that onair-gen's self-test can call it):
//  - RS41: the serial as it is (rs41mod: "S1234567").
//  - M20: "M20-" and the serial (m20mod: "M20-611-2-01234"; OnAir's serial is "611-2-01234").
//  - DFM: "DFM-" and the serial as dfm09mod's SN_out has it: "%6u" of the number (DFM-09/17) or "%6X" (DFM-06), so
//    space-padded to 6, then at most 9 characters (dfm09mod strncpy's 9 into its "DFM-xxxxxxxx" id). OnAir writes the number
//    with "%u" and a DFM-06's with "%06X", so its leading zeros become the padding: "17012345" is "DFM-17012345", "12345"
//    (DFM-09) and "012345" (DFM-06) are "DFM- 12345".
// A serial already carrying its prefix is left as it is.
inline std::string sondeId(const std::string& type, const std::string& serial) {
    auto has = [&serial](const char* p) { return serial.rfind(p, 0) == 0; };
    if (type == "M20") return has("M20-") ? serial : "M20-" + serial;
    if (type != "DFM" || has("DFM-")) return serial;
    std::string s = serial;
    while (s.size() > 1 && s[0] == '0') s.erase(0, 1);
    if (s.size() < 6) s.insert(0, 6 - s.size(), ' ');
    return "DFM-" + s.substr(0, 9);
}

} // namespace onair_rx
