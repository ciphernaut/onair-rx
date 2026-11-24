// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the status mode dvbs, the IP data of a DVB-S or DVB-S2 carrier. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's DvbsReceiver on the carrier nearest 0 Hz: it finds the carrier in the spectrum (its symbol rate from the spectrum's
// width and the cyclic line), hunts for DVB-S and DVB-S2 frames and decodes them on decoder threads of its own. The IP it
// carries goes to the receiver's IP sink (dvbsip::IpSink), which reassembles the datagrams and hands each one whole to OnAir's
// Wireshark hub (the dvbip channel): from GSE, which the receiver reads out of the DVB-S2 frames itself, and from MPE and ULE,
// which it reads out of the transport stream it is handed back (feedTs; the session hands it every packet it decodes). The
// line is a status line with the cadence of the other status modes (Cadence, poll.h): whether it is locked, the standard, the
// symbol rate and the MODCOD, which make a line when they change, and the SNR and the datagrams so far, which do not.
//
// The receiver drops frames when its decoder threads fall behind, as it does live. Unless --drop it is told to wait for them
// instead (setBlocking, its switch for recordings), and every poll waits until what was fed is decoded and delivered (flush),
// so that each line depends on the input alone. --width bounds the carrier: the input is cut to the band (BandFilter) before
// the receiver sees it.
#include "poll.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace onair_rx {

using namespace dect2;

namespace {

// The input cut to a band of width hz around 0 Hz: a linear-phase FIR low-pass (a Blackman-windowed sinc) run by overlap-save on
// OnAir's FFT. Its stop band starts at the band's edge (hz / 2) and its transition is 5 % of the band, so nothing outside the
// band reaches the receiver: a carrier that is wider is narrowed to it, and the spectrum search finds a carrier of at most the
// band, whose symbol rate is at most hz / (1 + roll-off). The taps are capped at kMaxTaps (a band narrower than about 2.7 % of
// the rate then has a wider transition, and one narrower than about 0.27 % lets a little past its edge). The output runs late
// by the filter's delay and by up to one FFT's step; flush() pushes the rest out at the end of input.
class BandFilter {
public:
    BandFilter(double rate, double hz) {
        const double trans = 0.05 * hz;
        const double want = 5.5 * rate / trans;   // Blackman: a transition of about 5.5 / taps of the rate
        const int taps = want >= kMaxTaps ? kMaxTaps : std::max(31, (int)std::ceil(want) | 1);
        int n = 1024;
        while (n < 4 * taps) n *= 2;
        n_ = n;
        taps_ = taps;
        // the cut-off, half a transition inside the band's edge (at least half the band's half width)
        const double fc = std::max(hz / 4, hz / 2 - 2.75 * rate / taps) / rate;
        std::vector<double> h((size_t)taps);
        double sum = 0;
        for (int i = 0; i < taps; i++) {
            const double t = i - (taps - 1) / 2.0, w = 2 * M_PI * i / (taps - 1);
            const double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
            h[(size_t)i] = sinc * (0.42 - 0.5 * std::cos(w) + 0.08 * std::cos(2 * w));
            sum += h[(size_t)i];
        }
        H_.assign((size_t)n, cf32());
        for (int i = 0; i < taps; i++) H_[(size_t)i] = cf32((float)(h[(size_t)i] / sum / n), 0.f);   // unit gain at 0 Hz; 1/n for the inverse
        fft_ = std::make_unique<Fft>(n);
        fft_->forward(H_.data());
        buf_.assign((size_t)n, cf32());
        have_ = (size_t)taps - 1;   // the history before the first sample: zeros
    }
    // what x makes of the filtered input, appended to out
    void run(const cf32* x, size_t n, std::vector<cf32>& out) {
        while (n > 0) {
            const size_t k = std::min(n, (size_t)n_ - have_);
            std::copy(x, x + k, buf_.begin() + (long)have_);
            have_ += k;
            x += k;
            n -= k;
            if (have_ < (size_t)n_) break;
            work_ = buf_;
            fft_->forward(work_.data());
            for (int i = 0; i < n_; i++) work_[(size_t)i] *= H_[(size_t)i];
            fft_->inverse(work_.data());
            out.insert(out.end(), work_.begin() + (taps_ - 1), work_.end());   // the samples whose history was all there
            std::copy(buf_.end() - (taps_ - 1), buf_.end(), buf_.begin());
            have_ = (size_t)taps_ - 1;
        }
    }
    // the rest of the input, with the filter's delay, at the end of input (zeros after it)
    void flush(std::vector<cf32>& out) {
        const size_t pending = have_ - ((size_t)taps_ - 1) + (size_t)(taps_ - 1) / 2;
        const std::vector<cf32> zeros((size_t)n_);
        const size_t before = out.size();
        while (out.size() - before < pending) run(zeros.data(), (size_t)n_ - have_, out);
        out.resize(before + pending);
    }
private:
    static constexpr int kMaxTaps = 4095;
    int n_ = 0, taps_ = 0;
    std::unique_ptr<Fft> fft_;
    std::vector<cf32> H_, buf_, work_;
    size_t have_ = 0;   // samples in buf_, the history of taps_ - 1 first
};

// The standard as the line names it (DvbsTelemetry::standard); nullptr while it is not known
const char* standardName(int s) { return s == 1 ? "dvbs" : s == 2 ? "dvbs2" : s == 3 ? "dvbs2x" : nullptr; }

class DvbsSession : public PollSession {
public:
    explicit DvbsSession(double rate) : cad_(rate) {}
    void open(const Options& o) {
        // the receiver's decoders on the CPU, as everywhere OnAir has no GPU decoder for: the same lines everywhere
        rx_.setComputeMode(0);
        rx_.setBlocking(!o.drop);
        rx_.configure(o.rate);
        drop_ = o.drop;
        if (o.width > 0 && o.width < o.rate) band_ = std::make_unique<BandFilter>(o.rate, o.width);
        // The transport stream the receiver decodes (DVB-S, and DVB-S2 carrying one) handed back to it, for MPE and ULE (the
        // engine's way, which the receiver's header names): on the receiver's decoder thread, outside its IP lock
        rx_.setPacketCallback([this](const uint8_t* p, size_t n, double) { rx_.feedTs(p, n); });
    }
    void feed(const cf32* x, size_t n) override {
        cad_.fed(n);
        if (!band_) { rx_.feed(x, n); return; }
        cut_.clear();
        band_->run(x, n, cut_);
        rx_.feed(cut_.data(), cut_.size());
    }
    void poll(bool last, std::vector<Json>& lines) override {
        if (last && band_) {
            cut_.clear();
            band_->flush(cut_);
            rx_.feed(cut_.data(), cut_.size());
        }
        if (!drop_ || last) rx_.flush();   // what was fed, decoded and delivered (its snapshot published)
        if (rx_.telemetry(tel_, seq_)) seq_ = tel_.seq;
        const char* standard = standardName(tel_.standard);
        const bool locked = standard && tel_.lockFec;   // the error correction delivers
        // The symbol rate the timing loop follows moves by tens of ppm from poll to poll: the line has it to 1 ksym/s, and the
        // last one written is kept while the loop stays within kRateHold of it
        if (!locked) rate_ = 0;
        else if (rate_ == 0 || std::fabs(tel_.symbolRate - (double)rate_) > kRateHold * (double)rate_)
            rate_ = std::llround(tel_.symbolRate / 1000) * 1000;
        // the MODCOD as OnAir's receiver window spells it (app/dvbs/dvbs_ui.cpp): "QPSK 2/3", " short" or " medium" frames,
        // " pilots"; " VCM" when it changes from frame to frame, which makes no line of its own then
        std::string modcod;
        if (locked && !tel_.modulationName.empty()) {
            modcod = tel_.modulationName + " " + tel_.codeRate;
            if (tel_.standard >= 2 && tel_.frameSize == 2) modcod += " short";
            else if (tel_.standard == 3 && tel_.frameSize == 3) modcod += " medium";
            if (tel_.pilots) modcod += " pilots";
            if (tel_.vcm) modcod += " VCM";
        }
        auto line = [&](bool measured) {
            Json j;
            j.flag("locked", locked);
            if (locked) {
                j.str("standard", standard).num("symbol_rate", rate_);
                if (!modcod.empty()) j.str("modcod", measured || !tel_.vcm ? modcod : std::string("VCM"));
                if (measured && std::isfinite(tel_.snrDb)) j.num("snr_db", (double)tel_.snrDb, 1);
            }
            if (measured) j.num("datagrams", (long long)tel_.ip.packets);
            return j;
        };
        cad_.offer(locked, last, line(false).done(), line(true), lines);
    }

private:
    static constexpr double kRateHold = 1e-3;
    DvbsReceiver rx_;
    DvbsTelemetry tel_;
    uint64_t seq_ = 0;
    bool drop_ = false;
    std::unique_ptr<BandFilter> band_;
    std::vector<cf32> cut_;
    long long rate_ = 0;   // the symbol rate the lines have, 0 while not locked
    Cadence cad_;
};

} // namespace

std::unique_ptr<PollSession> openDvbs(const Options& o, std::string* err) {
    if (o.rate < dvbsTuning().minSampleRate - 1) {   // DvbsReceiver::ready()'s own test, before a receiver is made
        char b[160];
        snprintf(b, sizeof b, "%.0f Hz is too low for dvbs (at least %.0f Hz)", o.rate, dvbsTuning().minSampleRate);
        *err = b;
        return nullptr;
    }
    // The receiver's decoders run on the CPU (setComputeMode(0)), but making one opens OnAir's GPU decoder all the same (its
    // ComputeChoice asks whether there is a GPU, which sets up Vulkan on the first graphics card): OnAir's own switch for that,
    // DECT2_NOGPU, keeps the GPU untouched. OnAir says so on stderr once.
    setenv("DECT2_NOGPU", "1", 1);
    auto s = std::make_unique<DvbsSession>(o.rate);
    s->open(o);
    return s;
}

} // namespace onair_rx
