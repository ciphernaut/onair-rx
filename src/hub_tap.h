// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the Wireshark hub's packets of one channel. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's receivers hand each frame to OnAir's Wireshark hub (patches/0001 gives it a sink, called on the publisher's thread).
// Most publish from inside feed(), on the thread that feeds them; some publish from a decoder thread of their own. The tap
// collects under a lock, so either works, and the read loop drains it after each block.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace onair_rx {

// The Wireshark hub's packets of one channel, collected from whatever thread publishes them. One per run.
class HubTap {
public:
    explicit HubTap(int chan);              // configures the hub (no file, no live capture, no GSMTAP), sets the sink, enables chan
    ~HubTap();                              // clears the sink and shuts the hub down
    HubTap(const HubTap&) = delete;
    HubTap& operator=(const HubTap&) = delete;
    void drain(std::vector<std::vector<uint8_t>>& out);   // moves out what has arrived since the last drain, in order (appended to out)

private:
    // shared with the sink: the hub calls a copy of its sink without its own lock, so a publisher may still be inside the sink
    // when the tap clears it; the queue lives until the last copy is gone
    struct Queue {
        std::mutex mu;
        std::vector<std::vector<uint8_t>> packets;
    };
    std::shared_ptr<Queue> q_;
};

// One packet as a pcap record body: as it is for a Direct channel, in IPv4 + UDP to the channel's port otherwise.
std::vector<uint8_t> hubRecord(int chan, const uint8_t* p, size_t n);

} // namespace onair_rx
