// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: AIS messages from OnAir's !AIVDM sentences. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The sentences are read by OnAir's own NMEA and AIS layer (ais::parseNmea, ais::unarmour, ais::decodeMessage); only the
// sequence id, which parseNmea does not return, is read here.
#include "ais.h"
#include "dect2/ais/ais_proto.h"
#include <cmath>
#include <cstdio>

namespace onair_rx {

using namespace dect2;

namespace {

// The sequence id of a sentence (its fourth field: "!AIVDM,2,1,3,B,..." has 3), empty when it has none
std::string seqId(const std::string& s) {
    size_t at = 0;
    for (int f = 0; f < 3; f++) {
        at = s.find(',', at);
        if (at == std::string::npos) return std::string();
        at++;
    }
    return s.substr(at, s.find(',', at) - at);
}

// a number to `decimals` places, written without its trailing zeros (a whole number without a decimal point); a non-finite
// one is left out
void decimal(Json& j, const char* k, double v, int decimals) {
    if (!std::isfinite(v)) return;
    char b[48];
    snprintf(b, sizeof b, "%.*f", decimals, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    j.raw(k, s);
}

} // namespace

std::optional<std::vector<std::string>> AisJoiner::add(const std::string& sentence) {
    std::string s = sentence;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::string chars;
    int fill = 0, count = 0, no = 0;
    char ch = 0;
    if (!ais::parseNmea(s, chars, fill, count, no, &ch) || count < 1 || no < 1 || no > count) return std::nullopt;
    auto forget = [this, ch] {   // the channel's messages still open
        for (auto it = open_.begin(); it != open_.end();) it = it->first.first == ch ? open_.erase(it) : std::next(it);
    };
    if (count == 1) {
        forget();
        return std::vector<std::string>{s};
    }
    const auto key = std::make_pair(ch, seqId(s));
    if (no == 1) {
        open_[key] = Open{count, {s}};
        return std::nullopt;
    }
    const auto it = open_.find(key);
    if (it == open_.end()) return std::nullopt;
    if (it->second.count != count || (int)it->second.parts.size() != no - 1) {   // not the next part: the message is lost
        open_.erase(it);
        return std::nullopt;
    }
    it->second.parts.push_back(s);
    if (no < count) return std::nullopt;
    std::vector<std::string> done = std::move(it->second.parts);
    open_.erase(it);
    forget();
    return done;
}

bool aisLine(const std::vector<std::string>& sentences, Json& j) {
    std::string payload;
    int fill = 0;
    char ch = 0;
    for (const std::string& s : sentences) {
        std::string chars;
        int count = 0, no = 0;
        if (!ais::parseNmea(s, chars, fill, count, no, &ch)) return false;
        payload += chars;
    }
    ais::AisMsg m;
    ais::decodeMessage(ais::unarmour(payload, fill), m);   // valid or not, the header is filled when the bits hold one
    if (m.bits < 38) return false;
    j.num("mmsi", (long long)m.mmsi).num("msg_type", (long long)m.type);
    if (ch) j.str("channel", std::string(1, ch));
    j.num("repeat", (long long)m.repeat).arr("nmea", sentences);
    // what the message type carries, by OnAir's not-available values: no position (hasPos), speed and course -1, heading -1,
    // navigational status 15 ("not defined"), IMO number 0, ship type 0, a text left empty
    if (m.hasPos) { decimal(j, "lat", m.lat, 6); decimal(j, "lon", m.lon, 6); }
    if (m.sog >= 0) decimal(j, "sog_kt", m.sog, 1);
    if (m.cog >= 0) decimal(j, "cog_deg", m.cog, 1);
    if (m.heading >= 0) j.num("heading_deg", (long long)m.heading);
    if (m.navStatus >= 0 && m.navStatus < 15) j.num("nav_status", (long long)m.navStatus);
    if (!m.name.empty()) j.str("name", m.name);
    if (!m.callsign.empty()) j.str("callsign", m.callsign);
    if (!m.destination.empty()) j.str("destination", m.destination);
    if (m.imo) j.num("imo", (long long)m.imo);
    if (m.shipType > 0) j.num("ship_type", (long long)m.shipType);
    return true;
}

} // namespace onair_rx
