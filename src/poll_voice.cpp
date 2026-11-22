// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the voice modes dmr, p25 and m17. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's DMR, P25 phase 1 and M17 receivers decode the channel at 0 Hz of the input and keep a log of the calls they heard
// in their snapshot, published every 0.25 s of signal: the calls that ended, the oldest aged out, and the
// one going on. Each mode reads that log at every poll and hands what it says about each call to a CallLog (calls.h), which
// writes a line when a call is first seen, when a member of it changes and when it ends. A call is keyed by where it was heard
// (the carrier, and the time slot for DMR) and when it started, in signal time: the log holds a call that ended for a long
// time, and a call sent again (a repeated capture) starts later, so each pass of it gives its own first and last line.
//
// A call's first line waits until its source, its destination and its colour code (DMR) or NAC (P25) are in hand (M17: the
// source, the destination and the type), as the node's rows have their first event: a call the log has without them is left
// out until they come.
//
// DMR and P25 also write a channel line (a channel on the air with no call) where the snapshot says so of the moment, not
// only that a colour code or NAC was once heard: a DMR repeater's idle or control bursts, a P25 control channel's blocks.
//
// No speech: the receivers are silent (no sound device) and are given a voice monitor of their own that refuses every call (its
// allow list is on and empty), which is OnAir's gate before the vocoder: no frame is decoded, whether mbelib or Codec 2 is
// installed or not, and the snapshot's call log is the same either way.
#include "calls.h"
#include "poll.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/voice/voice_monitor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace onair_rx {

using namespace dect2;

namespace {

constexpr double kDmrMinRate = 48e3;    // configureChannel: a channel already cut out, any rate from 48 kHz (dmr_rx.h)
constexpr double kP25MinRate = 250e3;   // p25Tuning().minSampleRate
constexpr double kM17MinRate = 250e3;   // m17Tuning().minSampleRate

bool tooLow(const char* mode, double rate, double least, std::string* err) {
    char b[160];
    snprintf(b, sizeof b, "%.0f Hz is too low for %s (at least %.0f Hz)", rate, mode, least);
    *err = b;
    return false;
}

// where and when a call was heard, exactly (%a: the start time's bits, so the same call always gives the same key)
std::string callKey(int carrier, int slot, double startSec) {
    char b[64];
    snprintf(b, sizeof b, "%d/%d/%a", carrier, slot, startSec);
    return b;
}

// What the voice modes share: the monitor that keeps the speech undecoded, the call log, and the poll.
class VoiceSession : public PollSession {
public:
    VoiceSession() {
        // A monitor of the wrapper's own (not OnAir's per-mode one; its mode name is none of OnAir's, so it hands nothing to
        // OnAir's session store): with its allow list on and empty, every call is Filtered, and a receiver decodes a voice
        // frame only for a call its monitor gives Play.
        voice::Settings s;
        s.allowOnly = true;
        mon_.setSettings(s);
    }
    void poll(bool last, std::vector<Json>& lines) override {
        if (snapshot() || last) {   // between two of the receiver's snapshots the last one stands: nothing new to say
            std::vector<CallSeen> now;
            calls(now);
            log_.see(now, lines);
            Json c;
            if (channel(c) && c.done() != channel_) {
                channel_ = c.done();
                lines.push_back(std::move(c));
            }
        }
        if (last) log_.finish(lines);
    }

protected:
    virtual bool snapshot() = 0;                         // the receiver's newest snapshot, false when it has none newer
    virtual void calls(std::vector<CallSeen>& now) = 0;  // what it says about each call
    // A channel line's members, when the snapshot shows the channel on the air with no call. It is written when it first
    // holds and again when what it says changes, not once per poll nor again after a call, as the node's rows have it.
    virtual bool channel(Json&) { return false; }
    voice::Monitor mon_{"onair-rx"};                     // before the receivers (the derived classes'), so it outlives them
    CallLog log_;
    std::string channel_;                                // the last channel line's members
};

// a channel line's members as the node's rows spell them: "network" is the text their summary shows for it
// (key: color_code or nac; name: how the text names it)
Json channelMembers(const char* key, const char* name, int value, bool control) {
    Json m;
    m.str("type", "channel").num(key, (long long)value).flag("control", control);
    char b[64];
    snprintf(b, sizeof b, "%s, %s %d", control ? "control channel" : "channel", name, value);
    return m.str("network", b);
}

// the members every call of DMR and P25 has, as the node's rows spell them; the reason for no speech only when it is the
// call's own (the receiver decodes none, and the node's other reasons name a vocoder)
Json callMembers(long long src, long long dst, bool group, bool encrypted, bool emergency) {
    Json m;
    m.str("type", "call").num("src", src).num("dst", dst).str("call", group ? "group" : "private");
    m.flag("encrypted", encrypted).flag("emergency", emergency);
    if (encrypted) m.str("silent", "encrypted");
    return m;
}

// --- DMR: DmrReceiver through configureChannel, the call log of DmrTelemetry (oldest first, the calls going on last)
class DmrSession : public VoiceSession {
public:
    bool open(const Options& o, std::string* err) {
        rx_.setSilent(true);
        rx_.setVoiceMonitor(&mon_);
        rx_.configureChannel(o.rate);
        if (!rx_.ready()) return tooLow("dmr", o.rate, kDmrMinRate, err);
        return true;
    }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }

private:
    bool snapshot() override {
        if (!rx_.telemetry(tel_, seq_)) return false;
        seq_ = tel_.seq;
        return true;
    }
    void calls(std::vector<CallSeen>& now) override {
        for (const DmrCall& c : tel_.callLog) {
            if (c.kind > 2) continue;                       // a data message or a control block: not a call
            if (!c.idsKnown || c.cc < 0) continue;          // its first line waits for both ids and the colour code
            Json m = callMembers(c.src, c.dst, c.kind != 1, c.privacy, c.emergency);   // kind 2, an all call, is a group's
            m.num("color_code", (long long)c.cc);
            if (c.slot == 1 || c.slot == 2) m.num("slot", (long long)c.slot);
            if (!c.alias.empty()) m.utf8("alias", c.alias);
            now.push_back({callKey(c.carrierId, c.slot, c.startSec), std::move(m), !c.active});
        }
    }
    // The channel's colour code stays in the snapshot after the carrier has gone, so it alone says nothing about now. A time
    // slot's state does: bursts decoded on it within the last second, the latest idle (3) or control blocks (4). The channel is
    // on the air with no call when every slot with bursts has such a latest burst (a repeater's: both slots, so a call whose
    // start was missed on the other one does not pass for an idle channel) and no call is going on. Once control blocks were
    // seen on a colour code it is a control channel, as the node's row has it, so a channel that mixes them with idle bursts
    // does not give a line at every turn.
    bool channel(Json& m) override {
        if (tel_.cc < 0) return false;
        int quiet = 0;
        bool busy = false;
        for (const DmrSlot& s : tel_.slot) {
            if (!s.active) continue;
            if (s.state == 4) controlCc_ = tel_.cc;
            if (s.inCall || (s.state != 3 && s.state != 4)) busy = true;
            else quiet++;
        }
        for (const DmrCall& c : tel_.callLog) busy = busy || c.active;
        if (busy || quiet == 0 || (tel_.link == "base station" && quiet < 2)) return false;
        m = channelMembers("color_code", "colour code", tel_.cc, controlCc_ == tel_.cc);
        return true;
    }
    DmrReceiver rx_;
    DmrTelemetry tel_;
    uint64_t seq_ = 0;
    int controlCc_ = -1;   // the colour code control blocks were seen on
};

// --- P25 phase 1: P25Receiver, its call log (newest first) and the call going on (now, also once in the log after its end)
class P25Session : public VoiceSession {
public:
    bool open(const Options& o, std::string* err) {
        rx_.setSilent(true);
        rx_.setVoiceMonitor(&mon_);
        rx_.configure(o.rate);
        rx_.setSignalOffset(0);
        if (!rx_.ready()) return tooLow("p25", o.rate, kP25MinRate, err);
        return true;
    }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }

private:
    bool snapshot() override {
        if (!rx_.telemetry(tel_, seq_)) return false;
        seq_ = tel_.seq;
        return true;
    }
    void call(const P25Call& c, std::vector<CallSeen>& now) {
        if (c.kind != 0) return;                            // a voice grant (another channel's call) or a data packet
        if (c.src < 0 || c.dst < 0 || c.nac < 0) return;    // its first line waits for both ids and the NAC
        Json m = callMembers(c.src, c.dst, c.group, c.encrypted, c.emergency);
        m.num("nac", (long long)c.nac);
        // The algorithm and key ids come with the header, or with an LDU2 that changes them: a call joined late shows the
        // snapshot's default (clear, key 0) until then, which is not yet known, so they are left out of such a call while so.
        if (!c.lateEntry || c.algId != 0x80 || c.keyId != 0) m.num("algid", (long long)c.algId).num("kid", (long long)c.keyId);
        now.push_back({callKey(c.carrierId, 0, c.startSec), std::move(m), !c.active});
    }
    void calls(std::vector<CallSeen>& now) override {
        for (auto it = tel_.log.rbegin(); it != tel_.log.rend(); ++it) call(*it, now);   // oldest first
        if (tel_.now.active) call(tel_.now, now);
    }
    // The NAC stays in the snapshot after the carrier has gone; `control` says a control channel's blocks (TSBKs) came within
    // the last 3 s. A conventional channel sends nothing between calls, so only a control channel is a channel with no call.
    bool channel(Json& m) override {
        if (!tel_.control || tel_.nac < 0 || tel_.now.active) return false;
        m = channelMembers("nac", "NAC", tel_.nac, true);
        return true;
    }
    P25Receiver rx_;
    P25Telemetry tel_;
    uint64_t seq_ = 0;
};

// --- M17: M17Receiver, its transmission log (newest first) and the one going on (now, also once in the log after its end)

// An address as the node's row spells it: OnAir's "@ALL" (the broadcast address) is ALL, and an extended-range address,
// OnAir's "#" and twelve hex digits, is "#" and the base-40 digits of its offset above 0xEE6B28000000 (the node's spelling)
std::string m17Address(const std::string& a) {
    if (a == "@ALL") return "ALL";
    if (a.size() == 13 && a[0] == '#') {
        static const char* cs = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
        uint64_t v = std::strtoull(a.c_str() + 1, nullptr, 16) - 0xEE6B28000000ull;
        std::string s = "#";
        for (; v; v /= 40) s += cs[v % 40];
        return s;
    }
    return a;
}

class M17Session : public VoiceSession {
public:
    bool open(const Options& o, std::string* err) {
        rx_.setSilent(true);
        rx_.setVoiceMonitor(&mon_);
        rx_.configure(o.rate);
        rx_.setSignalOffset(0);
        if (!rx_.ready()) return tooLow("m17", o.rate, kM17MinRate, err);
        return true;
    }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }

private:
    bool snapshot() override {
        if (!rx_.telemetry(tel_, seq_)) return false;
        seq_ = tel_.seq;
        return true;
    }
    void call(const M17Transmission& t, std::vector<CallSeen>& now) {
        // the row's types from OnAir's text of the TYPE field ("voice 3200", "voice + data 1600", "data", "packet"; a packet SMS
        // is "SMS"); a reserved data type is data, as the node names it; a BERT stream is a test of the link, not a call
        std::string type;
        if (t.packet) type = "packet";
        else if (t.type.rfind("voice + data", 0) == 0) type = "voice_data";
        else if (t.type.rfind("voice", 0) == 0) type = "voice";
        else if (t.type == "data" || t.type == "reserved") type = "data";
        if (type.empty() || t.src.empty() || t.dst.empty()) return;   // its first line waits for both callsigns and the type
        Json m;
        m.str("src", m17Address(t.src)).str("dst", m17Address(t.dst));
        if (t.can >= 0) m.num("can", (long long)t.can);
        m.str("type", type);
        // OnAir's text of the encryption ("scrambler 8-bit", "AES-256", "other"; empty: clear) as the row's enumeration
        if (t.enc.rfind("scrambler", 0) == 0) m.str("encryption", "scrambler");
        else if (t.enc.rfind("AES", 0) == 0) m.str("encryption", "aes");
        else if (!t.enc.empty()) m.str("encryption", "other");
        if (t.hasGnss && std::isfinite(t.lat) && std::isfinite(t.lon)) m.num("lat", t.lat, 6).num("lon", t.lon, 6);
        now.push_back({callKey(t.carrierId, 0, t.startSec), std::move(m), !t.active});
    }
    void calls(std::vector<CallSeen>& now) override {
        for (auto it = tel_.log.rbegin(); it != tel_.log.rend(); ++it) call(*it, now);   // oldest first
        if (tel_.now.active) call(tel_.now, now);
    }
    M17Receiver rx_;
    M17Telemetry tel_;
    uint64_t seq_ = 0;
};

template <class S>
std::unique_ptr<PollSession> openVoice(const Options& o, std::string* err) {
    auto s = std::make_unique<S>();
    if (!s->open(o, err)) return nullptr;
    return s;
}

} // namespace

std::unique_ptr<PollSession> openDmr(const Options& o, std::string* err) { return openVoice<DmrSession>(o, err); }
std::unique_ptr<PollSession> openP25(const Options& o, std::string* err) { return openVoice<P25Session>(o, err); }
std::unique_ptr<PollSession> openM17(const Options& o, std::string* err) { return openVoice<M17Session>(o, err); }

} // namespace onair_rx
