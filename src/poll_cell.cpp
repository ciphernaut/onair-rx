// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the cell modes tetra, gsmr and lte. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's TETRA, GSM-R and LTE receivers read what base stations broadcast in the clear to every terminal on their downlink:
// TETRA the synchronisation burst (MCC, MNC, colour code), SYSINFO (location area) and the neighbour broadcast (neighbours,
// network time); GSM-R the SCH (BSIC) and System Information 1 to 4 (location area, cell identity); LTE the synchronisation
// signals (physical cell id), the MIB (bandwidth) and the system information on SI-RNTI (SIB1: MCC, MNC, tracking area, cell
// identity; the SIBs read). Each keeps a table of the cells it has heard in its snapshot. Each mode reads that table at every
// poll and hands what it says about each cell to a CellLog, which writes a line when a cell is first read and when a member of
// it changes, keyed by its carrier: a cell that stays on the air gives no line per poll.
//
// All three decode a stretch of input at a time on a worker thread of their own (TETRA 2.3 s, GSM-R 2.2 s, LTE 0.2 s) and skip
// the input that comes while it is busy, as they do live. A replay faster than real time would then lose most of its input, and
// which part would depend on the worker's timing. So, unless --drop, a session waits for the worker after each stretch it hands
// over, and the receiver skips nothing: the stretches follow each other from sample 0, each decoded before the next sample is
// fed, so every snapshot and every line depends on the input alone. GSM-R's and LTE's receivers also drop the rest of the feed
// that completes a stretch (gsmr_rx.cpp and cell_rx.cpp, feed): their sessions split their blocks at the stretch's end. The
// receivers have no flush, so at the end of input the session completes the last partial stretch with zeros, and the receiver
// decodes what it holds. The last poll waits at most kFlushSec in all (--drop: first for a stretch still being decoded, then
// for the padded one), and the session's end makes a stretch still being decoded give up at its next carrier.
//
// Under --drop the session does not wait, and what the receiver decodes is still contiguous within each stretch: the session
// gathers the input into whole stretches itself and hands one over only when the worker is idle, else drops it whole, as the
// receivers do live; a gap the read-ahead makes (its queue full, PollSession::gap) drops the part of a stretch gathered
// before it, and the next stretch starts after the gap.
//
// Their frames (TETRA's signalling blocks, GSM-R's BCCH blocks, LTE's system information messages) go to OnAir's Wireshark hub
// as GSMTAP, from the worker; runPolled collects them under the hub tap's lock.
#include "poll.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_tel.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace onair_rx {

using namespace dect2;

namespace {

// the last poll waits at most this long, for a stretch being decoded and the padded one: under ZetaSDR's 5 s grace for a
// handler to exit once its input is closed (zeta-ext, EXIT_GRACE), so that the lines of the end of input are written first
constexpr double kFlushSec = 3;
constexpr double kGsmrStretchSec = 2.2;   // GsmrReceiver's stretch (gsmr_rx.cpp, kBlockSec), which its header does not export
constexpr double kLteStretchSec = 0.2;    // CellReceiver's block (cell_rx.cpp, kBlockSec), which its header does not export either

bool tooLow(const char* mode, double rate, double least, std::string* err) {
    char b[160];
    snprintf(b, sizeof b, "%.0f Hz is too low for %s (at least %.0f Hz)", rate, mode, least);
    *err = b;
    return false;
}

using Clock = std::chrono::steady_clock;

// Waits until the receiver's worker is idle with no stretch waiting for it, at most until the deadline (none: as long as it
// takes, which a stretch's decoding always ends). False when it is still busy.
template <class Rx>
bool waitIdle(const Rx& rx, const Clock::time_point* until = nullptr) {
    while (rx.busy()) {
        if (until && Clock::now() >= *until) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return true;
}

// What a snapshot says about one cell. id: what the cell broadcasts as its identity; hz: its carrier, as the receiver has it;
// compared: the members a change of which gives a line (written out), without what is measured (the carrier as measured, the
// SNR) or a clock (TETRA's network time); line: the line's members.
struct CellSeen {
    std::string id;
    double hz;
    std::string compared;
    Json line;
};

// Turns successive snapshots of a receiver's cell table into lines: a cell first seen gives its line, a cell whose compared
// members changed gives it again. A cell is the same cell between snapshots when its identity is the same and its carrier is
// within sameHz of where it was (the receiver's own tolerance), so a carrier measured a few hertz off in each stretch stays one
// cell. A cell that is no longer heard gives no line (it stays in the receiver's table).
class CellLog {
public:
    explicit CellLog(double sameHz) : same_(sameHz) {}
    void see(std::vector<CellSeen>& now, std::vector<Json>& lines) {
        gen_++;
        std::stable_sort(now.begin(), now.end(), [](const CellSeen& a, const CellSeen& b) { return a.hz != b.hz ? a.hz < b.hz : a.id < b.id; });
        for (CellSeen& c : now) {
            Cell* k = nullptr;
            for (Cell& x : cells_)
                if (x.id == c.id && std::fabs(x.hz - c.hz) <= same_) { k = &x; break; }
            if (!k) {
                if (cells_.size() >= kMax)   // the one heard longest ago goes
                    cells_.erase(std::min_element(cells_.begin(), cells_.end(), [](const Cell& a, const Cell& b) { return a.seen < b.seen; }));
                cells_.push_back({c.id, c.hz, c.compared, gen_});
                lines.push_back(std::move(c.line));
                continue;
            }
            k->hz = c.hz;
            k->seen = gen_;
            if (k->compared == c.compared) continue;
            k->compared = c.compared;
            lines.push_back(std::move(c.line));
        }
    }

private:
    struct Cell {
        std::string id;
        double hz;
        std::string compared;
        uint64_t seen;   // the see() that last had it
    };
    static constexpr size_t kMax = 400;   // twice TETRA's and GSM-R's own table (kTetraMax, kGsmrMax), LTE's (kCellMax)
    std::vector<Cell> cells_;
    double same_;
    uint64_t gen_ = 0;
};

// What both modes share: the receiver, its snapshot, the cell log and the poll.
template <class Rx, class Tel>
class CellSession : public PollSession {
public:
    explicit CellSession(double sameHz) : log_(sameHz) {}
    // a stretch still being decoded (the last poll gave up waiting for it) gives up at its next carrier: the receiver's own end
    // then joins its worker at once
    ~CellSession() override { rx_.reset(); }
    void poll(bool last, std::vector<Json>& lines) override {
        if (last) flush();
        if (!rx_.telemetry(tel_, seq_)) return;   // between two snapshots the last one stands: nothing new to say
        seq_ = tel_.seq;
        std::vector<CellSeen> now;
        cells(now);
        log_.see(now, lines);
    }

protected:
    void gap(uint64_t) override { part_.clear(); }   // only under --drop: the part of a stretch before the gap goes
    // The end of input: the last partial stretch is completed with zeros, so that the receiver decodes what it holds, and
    // waited for, all within kFlushSec. Without --drop the worker is idle and what the receiver holds is known (fill_): nothing
    // is padded when the input ended with a whole stretch. With --drop a stretch may still be being decoded (it is waited
    // for first); the receiver holds nothing, and the part of a stretch the session gathered (part_) is completed with zeros
    // and handed over.
    void flush() {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(std::llround(kFlushSec * 1000));
        if (!waitIdle(rx_, &until)) return;
        if (drop_) {
            if (part_.empty()) return;
            part_.resize(want_);   // the rest zeros
            rx_.feed(part_.data(), part_.size());
            part_.clear();
            waitIdle(rx_, &until);
            return;
        }
        size_t pad = fill_ ? want_ - fill_ : 0;
        fill_ = 0;
        if (pad == 0) return;
        static const std::vector<cf32> zeros(kPollBlock);
        while (pad > 0) {
            const size_t k = std::min(pad, zeros.size());
            rx_.feed(zeros.data(), k);
            pad -= k;
        }
        waitIdle(rx_, &until);
    }
    // --drop: the input gathered into whole stretches (part_); a whole one goes to the receiver when its worker is idle and is
    // dropped whole when it is busy (as the receivers drop live), so that the receiver is never held up and never decodes a
    // stretch with a hole in it. The receiver then gathers it at once, in one feed, and holds nothing between two of them.
    void feedDrop(const cf32* x, size_t n) {
        while (n > 0) {
            const size_t take = std::min(n, want_ - part_.size());
            part_.insert(part_.end(), x, x + take);
            x += take;
            n -= take;
            if (part_.size() < want_) continue;
            if (!rx_.busy()) rx_.feed(part_.data(), part_.size());
            part_.clear();
        }
    }
    // For a receiver that gathers a stretch only while its worker is idle and drops the rest of the feed that completes one
    // (GSM-R, LTE): the block is cut at the stretch's end and the worker waited for there, so it is idle at every feed and
    // nothing is skipped. Under --drop the stretches are gathered by the session (feedDrop).
    void feedCut(const cf32* x, size_t n) {
        if (drop_) { feedDrop(x, n); return; }
        while (n > 0) {
            const size_t take = std::min(n, want_ - fill_);
            rx_.feed(x, take);
            x += take;
            n -= take;
            fill_ += take;
            if (fill_ < want_) continue;
            fill_ = 0;
            waitIdle(rx_);
        }
    }
    virtual void cells(std::vector<CellSeen>& now) = 0;   // what the snapshot says about each cell
    // the SNR as the line has it: dB to a tenth, left out when the receiver has not measured it (-99)
    static void snrDb(Json& j, float db) { if (std::isfinite(db) && db > -99) j.num("snr_db", (double)db, 1); }
    // --drop, once want_ is known: the session gathers the stretches (part_)
    void setDrop(bool drop) {
        drop_ = drop;
        if (drop_) part_.reserve(want_);
    }
    bool drop_ = false;
    size_t want_ = 0, fill_ = 0;   // a stretch's samples (the receiver's own expression); those of the current one fed so far
    std::vector<cf32> part_;       // --drop: the stretch being gathered
    Rx rx_;
    Tel tel_;
    uint64_t seq_ = 0;
    CellLog log_;
};

// --- TETRA: TetraReceiver searching the whole input for 25 kHz carriers (its default; no single carrier), told the radio
// frequency (--freq) so that each carrier has its own
class TetraSession : public CellSession<TetraReceiver, TetraTelemetry> {
public:
    TetraSession() : CellSession(5e3) {}   // tetra_rx.cpp, merge(): a carrier within 5 kHz of a known cell is that cell
    bool open(const Options& o, std::string* err) {
        rx_.configure(o.rate);
        if (!rx_.ready()) return tooLow("tetra", o.rate, tetraTuning().minSampleRate, err);
        rx_.setCenterHz(o.freq);
        want_ = (size_t)(o.rate * TetraReceiver::kCaptureSec);   // configure()'s own expression
        setDrop(o.drop);
        return true;
    }
    // The receiver gathers the next stretch while it decodes one, and skips what comes once that one is full and the worker
    // still busy. One block never holds two stretches' ends, so waiting after each block is enough for it never to skip.
    void feed(const cf32* x, size_t n) override {
        if (drop_) { feedDrop(x, n); return; }
        rx_.feed(x, n);
        fill_ += n;
        if (fill_ >= want_) fill_ -= want_;
        waitIdle(rx_);
    }

private:
    void cells(std::vector<CellSeen>& now) override {
        for (const TetraCell& c : tel_.cells) {
            if (c.mcc < 0 || c.mnc < 0 || c.colourCode < 0) continue;   // the receiver keeps no cell without an identity
            // the frequency the cell announces as its main carrier (SYSINFO) when this is it, else the carrier as measured
            Json j, k;
            j.num("freq_hz", std::llround(c.nominalHz));
            for (Json* m : {&j, &k}) {
                m->num("mcc", (long long)c.mcc).num("mnc", (long long)c.mnc).num("colour_code", (long long)c.colourCode);
                if (c.hasSysinfo && c.la >= 0) m->num("la", (long long)c.la);
            }
            if (!c.netTime.empty()) j.utf8("network_time", c.netTime);
            k.flag("network_time", !c.netTime.empty());   // that the broadcast carries a time, not the time itself
            if (c.hasNwrk) {
                j.num("neighbours", (long long)c.neighbours.size());
                k.num("neighbours", (long long)c.neighbours.size());
            }
            snrDb(j, c.snrDb);
            char id[48];
            snprintf(id, sizeof id, "%d/%d/%d", c.mcc, c.mnc, c.colourCode);
            now.push_back({id, c.freqHz, k.done(), std::move(j)});
        }
    }
};

// --- GSM-R: GsmrReceiver searching the whole input for 200 kHz carriers, told the radio frequency (--freq) so that each
// carrier has its ARFCN
class GsmrSession : public CellSession<GsmrReceiver, GsmrTelemetry> {
public:
    GsmrSession() : CellSession(60e3) {}   // gsmr_rx.cpp, merge(): a carrier off the raster within 60 kHz is the same cell
    bool open(const Options& o, std::string* err) {
        rx_.configure(o.rate);
        if (!rx_.ready()) return tooLow("gsmr", o.rate, gsmrTuning().minSampleRate, err);
        rx_.setCenterHz(o.freq);
        want_ = (size_t)(o.rate * kGsmrStretchSec);   // the receiver's own expression (configure()), so the same number
        setDrop(o.drop);
        return true;
    }
    // The receiver gathers a stretch only while its worker is idle, and drops the rest of the feed that completes one
    void feed(const cf32* x, size_t n) override { feedCut(x, n); }

private:
    void cells(std::vector<CellSeen>& now) override {
        for (const GsmrCell& c : tel_.cells) {
            if (c.bsic < 0) continue;
            Json j, k;
            j.num("freq_hz", std::llround(c.freqHz));   // the ARFCN's own frequency (on the raster), else as measured
            for (Json* m : {&j, &k}) {
                if (c.arfcn >= 0) m->num("arfcn", (long long)c.arfcn);
                m->num("bsic", (long long)c.bsic);
                if (c.hasLai) m->num("mcc", (long long)c.mcc).num("mnc", (long long)c.mnc).num("lac", (long long)c.lac);
                if (c.hasSi3) m->num("cell_id", (long long)c.cellId);
            }
            snrDb(j, c.snrDb);
            char id[48];
            snprintf(id, sizeof id, "%d/%d", c.arfcn, c.bsic);
            now.push_back({id, c.freqHz, k.done(), std::move(j)});
        }
    }
};

// --- LTE: CellReceiver searching the input for LTE carriers, told the radio frequency (--freq) so that it also looks for a cell on
// every 100 kHz raster point the input holds (carriers side by side make one block in the spectrum) and each cell has its
// raster frequency
class LteSession : public CellSession<CellReceiver, CellTelemetry> {
public:
    LteSession() : CellSession(60e3) {}   // cell_rx.cpp, merge(): the same PCI within 60 kHz of a known cell is that cell
    bool open(const Options& o, std::string* err) {
        rx_.configure(o.rate);
        if (!rx_.ready()) return tooLow("lte", o.rate, cellTuning().minSampleRate, err);
        rx_.setCenterHz(o.freq);
        want_ = (size_t)(o.rate * kLteStretchSec);   // configure()'s own expression
        setDrop(o.drop);
        return true;
    }
    // The receiver gathers a block only while its worker is idle, and drops the rest of the feed that completes one
    void feed(const cf32* x, size_t n) override { feedCut(x, n); }

private:
    // the channel bandwidth of the MIB's resource block count (36.101 table 5.6-1), 0 for a count no bandwidth has
    static long long bandwidthHz(int nRb) {
        switch (nRb) {
        case 6: return 1400000;
        case 15: return 3000000;
        case 25: return 5000000;
        case 50: return 10000000;
        case 75: return 15000000;
        case 100: return 20000000;
        default: return 0;
        }
    }
    void cells(std::vector<CellSeen>& now) override {
        for (const CellTower& c : tel_.cells) {
            Json j, k;
            j.num("freq_hz", std::llround(c.freqHz));   // the raster frequency of the cell's carrier
            for (Json* m : {&j, &k}) {
                m->num("pci", (long long)c.pci);
                if (c.hasMib && bandwidthHz(c.nRb)) m->num("bandwidth_hz", bandwidthHz(c.nRb));
                if (c.hasSib1 && c.mcc >= 0) {
                    m->num("mcc", (long long)c.mcc).num("mnc", (long long)c.mnc);
                    m->num("tac", (long long)c.tac).num("cell_id", (long long)c.cellId);
                }
                if (!c.sibsSeen.empty()) {   // ascending (the receiver keeps them sorted)
                    std::string a;
                    for (int n : c.sibsSeen) a += (a.empty() ? "[" : ",") + std::to_string(n);
                    m->raw("sibs", a + "]");
                }
            }
            snrDb(j, c.sinrDb);
            now.push_back({std::to_string(c.pci), c.freqHz, k.done(), std::move(j)});
        }
    }
};

template <class S>
std::unique_ptr<PollSession> openCell(const Options& o, std::string* err) {
    auto s = std::make_unique<S>();
    if (!s->open(o, err)) return nullptr;
    return s;
}

} // namespace

std::unique_ptr<PollSession> openTetra(const Options& o, std::string* err) { return openCell<TetraSession>(o, err); }
std::unique_ptr<PollSession> openGsmr(const Options& o, std::string* err) { return openCell<GsmrSession>(o, err); }
std::unique_ptr<PollSession> openLte(const Options& o, std::string* err) { return openCell<LteSession>(o, err); }

} // namespace onair_rx
