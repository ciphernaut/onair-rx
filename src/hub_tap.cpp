// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the Wireshark hub's packets of one channel. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#include "hub_tap.h"
#include "dect2/wirecap/wire.h"

namespace onair_rx {

namespace wire = dect2::wire;

HubTap::HubTap(int chan) : q_(std::make_shared<Queue>()) {
    wire::Config cfg;   // no file, no live capture, no GSMTAP: the sink is the hub's only output
    cfg.dir.clear();
    cfg.files = cfg.live = cfg.gsmtap = false;
    wire::Hub::get().configure(cfg);
    std::shared_ptr<Queue> q = q_;
    wire::Hub::get().setSink([q, chan](wire::Chan c, int64_t, const uint8_t* p, size_t n) {
        if (c != chan) return;
        std::lock_guard<std::mutex> lk(q->mu);
        q->packets.emplace_back(p, p + n);
    });
    wire::Hub::get().setEnabled((wire::Chan)chan, true);
}

HubTap::~HubTap() {
    wire::Hub::get().setSink({});
    wire::Hub::get().shutdown();
}

void HubTap::drain(std::vector<std::vector<uint8_t>>& out) {
    std::vector<std::vector<uint8_t>> got;
    {
        std::lock_guard<std::mutex> lk(q_->mu);
        got.swap(q_->packets);
    }
    if (out.empty()) { out.swap(got); return; }
    for (auto& p : got) out.push_back(std::move(p));
}

std::vector<uint8_t> hubRecord(int chan, const uint8_t* p, size_t n) {
    // a packet the channel has as application data is wrapped as OnAir's capture files have it (wire.cpp): in IPv4 + UDP to the
    // channel's port, so that the port's dissector reads it
    const wire::ChanInfo& ci = wire::kChans[chan];
    if (ci.encap == wire::Encap::Direct) return std::vector<uint8_t>(p, p + n);
    return wire::udpInIpv4(ci.udpPort, p, n);
}

} // namespace onair_rx
