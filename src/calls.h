// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: a voice receiver's call log, as lines. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// The voice receivers (DMR, P25, M17) keep a log of calls in their snapshot: a list that grows, ages out its oldest entries,
// and holds an ended call for many polls after its end. A mode turns each snapshot into what it says about each call (CallSeen)
// and a CallLog turns successive snapshots into lines: one when a call is first seen, one when a member of it changes, one with
// "eot": true when it ends, so a call that stays up gives no line per poll and an ended call that stays in the log gives none.
// Header-only, so that onair-gen's self-test drives it as the modes do.
#pragma once
#include "rx.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace onair_rx {

// What a snapshot says about one call. key: what makes it the same call between polls (the mode's own choice: the voice modes
// take where it was heard and when it started, so a call sent again is a new call); members: the line's members as written,
// without "eot", which the log adds; ended: the snapshot marks it over.
struct CallSeen { std::string key; Json members; bool ended; };

// Turns successive snapshots into lines: a call first seen gives its members; a call whose members changed gives them again; a
// call marked ended, or no longer in the snapshot, gives its last members with "eot": true, once (a call first seen already
// ended gives both lines at once). A call seen again open after its end is a new call. finish() ends every call still open
// (the last poll). Every line has "eot": false but the last one of its call, as the node's rows have it.
class CallLog {
public:
    void see(const std::vector<CallSeen>& now, std::vector<Json>& lines) {
        gen_++;
        std::vector<Json> fresh;
        for (const CallSeen& c : now) {
            auto it = calls_.find(c.key);
            if (it != calls_.end() && it->second.seen == gen_) continue;   // the same call twice in one snapshot: its first stands
            if (it == calls_.end() || (it->second.ended && !c.ended)) {   // a new call
                fresh.push_back(line(c.members, false));
                if (c.ended) fresh.push_back(line(c.members, true));
                calls_[c.key] = Call{c.members, c.members.done(), c.ended, gen_, order_++};
                continue;
            }
            Call& k = it->second;
            k.seen = gen_;
            if (k.ended) continue;   // over, and still in the log: said already
            const std::string text = c.members.done();
            if (c.ended) fresh.push_back(line(c.members, true));
            else if (text != k.text) fresh.push_back(line(c.members, false));
            k.members = c.members;
            k.text = text;
            k.ended = c.ended;
        }
        // what the snapshot no longer holds: an open call ends with the members it had, in the order the calls were first seen
        std::vector<std::pair<uint64_t, std::string>> gone;
        for (const auto& [key, k] : calls_)
            if (k.seen != gen_) gone.emplace_back(k.order, key);
        std::sort(gone.begin(), gone.end());
        for (const auto& [order, key] : gone) {
            auto it = calls_.find(key);
            if (!it->second.ended) lines.push_back(line(it->second.members, true));
            calls_.erase(it);
        }
        for (Json& l : fresh) lines.push_back(std::move(l));
    }

    void finish(std::vector<Json>& lines) {
        std::vector<std::pair<uint64_t, std::string>> open;
        for (const auto& [key, k] : calls_)
            if (!k.ended) open.emplace_back(k.order, key);
        std::sort(open.begin(), open.end());
        for (const auto& [order, key] : open) {
            Call& k = calls_[key];
            lines.push_back(line(k.members, true));
            k.ended = true;
        }
    }

private:
    struct Call {
        Json members;
        std::string text;      // members.done(), to compare
        bool ended;
        uint64_t seen;         // the see() that last had it
        uint64_t order;        // when it was first seen
    };
    static Json line(const Json& members, bool eot) { Json j = members; j.flag("eot", eot); return j; }
    std::unordered_map<std::string, Call> calls_;
    uint64_t gen_ = 0, order_ = 0;
};

} // namespace onair_rx
