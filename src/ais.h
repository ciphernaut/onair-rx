// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: AIS messages from OnAir's !AIVDM sentences. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's AisReceiver hands each message it decodes to OnAir's Wireshark hub as NMEA 0183 sentences, one packet each (a
// message longer than one sentence's 60 characters of payload is cut into several, which share a sequence id). The ais mode
// makes its lines from those: the joiner puts a message's sentences back together, and the line is what OnAir's own AIS
// parser (dect2/ais/ais_proto.h) reads from them.
#pragma once
#include "rx.h"
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace onair_rx {

// Joins the sentences of a message, kept apart by channel and sequence id. A part that does not follow the part before it of
// the same channel and sequence id ends that message unbuilt; a complete message on a channel forgets the channel's parts of
// messages still open (OnAir publishes a message's sentences one after the other, so such a part's rest is not coming).
class AisJoiner {
public:
    // A sentence (a line end, if any, is taken off): the message's sentences, without line ends, when it completes one
    std::optional<std::vector<std::string>> add(const std::string& sentence);
private:
    struct Open { int count = 0; std::vector<std::string> parts; };
    std::map<std::pair<char, std::string>, Open> open_;   // by channel and sequence id
};

// The members of the line of a message from its sentences: mmsi, msg_type, channel, repeat, nmea, and what the message type
// carries of lat, lon, sog_kt, cog_deg, heading_deg, nav_status, name, callsign, destination, imo and ship_type (a value the
// message gives as not available is left out). False when the sentences do not hold a message's header.
bool aisLine(const std::vector<std::string>& sentences, Json& j);

} // namespace onair_rx
