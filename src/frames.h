// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: lines made from Wireshark hub packets (zigbee, thread, zwave, wifi, wisun). Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// These five receivers put nothing on OnAir's automation bus. What they decode goes to OnAir's Wireshark hub, one packet per
// frame whose check passed: the 802.15.4 PSDU with its FCS (Zigbee, Thread), the data rate and the MPDU (Z-Wave), a radiotap
// header and the 802.11 frame (Wi-Fi), the 802.15.4 MPDU without its FCS (Wi-SUN). Each mode's line is what OnAir's own parser
// reads from its packets (zigbee::parse and thread::analyse, zwave::parse, wifi::parseFrame, wisun::parse); a packet the parser
// cannot read gives no line and is still a pcap record. Each function returns what adds one packet's members to its line (false:
// no line for that packet), once per run (BusMode::packetLines).
#pragma once
#include "bus.h"
#include <memory>
#include <string>

namespace dect2 { class WisunReceiver; }

namespace onair_rx {

PacketLine zigbeePackets();
PacketLine threadPackets();
PacketLine wifiPackets();
// Z-Wave's packets carry the data rate but not the channel: the line's freq_hz is the one channel of the region (o.region, or
// zwaveRegion's pick) that the input covers and that uses the frame's rate
PacketLine zwavePackets(const Options& o, const Feed& feed);
// Wi-SUN's packets carry the MPDU only: the line's channel and bit rate are those of the frame in the receiver's frame log, its
// last report (left out when the report does not have the frame yet). rx: the run's receiver, whose report is read on the
// thread that feeds it
PacketLine wisunPackets(std::shared_ptr<dect2::WisunReceiver> rx);

// The Z-Wave region (an id of OnAir's table, "EU"...) to listen to at this centre and rate: the region named (case does not
// matter; "" for OnAir's own pick, the region with most channels inside the input, the first winning a tie). False with *err set
// when the name is none of the table's, or the region has no channel inside the input (a receiver that would never hear a frame).
bool zwaveRegion(double centreHz, double rate, const std::string& name, std::string* id, std::string* err);

} // namespace onair_rx
