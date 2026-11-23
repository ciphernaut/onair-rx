// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: lines made from Wireshark hub packets (zigbee, thread, zwave, wifi). Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// These four receivers put nothing on OnAir's automation bus. What they decode goes to OnAir's Wireshark hub, one packet per
// frame whose check passed: the 802.15.4 PSDU with its FCS (Zigbee, Thread), the data rate and the MPDU (Z-Wave), a radiotap
// header and the 802.11 frame (Wi-Fi). Each mode's line is what OnAir's own parser reads from its packets (zigbee::parse and
// thread::analyse, zwave::parse, wifi::parseFrame); a packet the parser cannot read gives no line and is still a pcap record.
// Each function is a BusMode::packetLines: called once per run, it returns what adds one packet's members to its line (false:
// no line for that packet).
#pragma once
#include "rx.h"
#include <functional>
#include <string>

namespace onair_rx {

std::function<bool(const uint8_t*, size_t, Json&)> zigbeePackets();
std::function<bool(const uint8_t*, size_t, Json&)> threadPackets();
std::function<bool(const uint8_t*, size_t, Json&)> zwavePackets();
std::function<bool(const uint8_t*, size_t, Json&)> wifiPackets();

// Z-Wave's packets carry the data rate but not the channel: the line's freq_hz is the one channel of the region that the input
// covers and that uses the frame's rate. This says where the input is, before zwavePackets() is called: the radio frequency at 0
// Hz, the sample rate and the region's id.
void zwaveWhere(double centreHz, double rate, const std::string& region);

// The Z-Wave region (an id of OnAir's table, "EU"...) to listen to at this centre and rate: the region named (case does not
// matter; "" for OnAir's own pick, the region with most channels inside the input, the first winning a tie). False with *err set
// when the name is none of the table's, or the region has no channel inside the input (a receiver that would never hear a frame).
bool zwaveRegion(double centreHz, double rate, const std::string& name, std::string* id, std::string* err);

} // namespace onair_rx
