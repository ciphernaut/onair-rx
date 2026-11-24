# What to offer upstream

Changes to OnAir, as the operator would send them. Nothing here has been sent. OnAir takes focused pull requests
with a test and a DCO sign-off (`git commit -s`), from the operator's account, one change to a request.

Based on OnAir by Kristijonas Adomaitis - https://on-air.dev

## 1. A sink on the Wireshark hub

Written: `patches/0001-wirecap-hub-sink.patch`, which `build.sh` applies to the copy of OnAir it builds.

- **Change:** `wire::Hub::setSink`. The sink sees each packet published on an enabled channel, on the publisher's
  thread, with the time exactly as the caller gave it. With the hub's files, live capture and GSMTAP off, the hub opens
  no file and no socket. This lets a program take a receiver's frames in process, which `onair-rx` does for its pcap
  stream and for AIS.
- **Test:** `tests/wirecap/test_wire_sink.cpp`, in OnAir's style. `test/patch.sh` compiles and runs it against the
  built libraries.
- **Ready to offer (2026-11-24):** the branch `wirecap-hub-sink` on the operator's fork
  (<https://github.com/ciphernaut/OnAir/tree/wirecap-hub-sink>) is this patch on OnAir 4.6.0. Built there in OnAir's own
  test build (`DECT2_BUILD_TESTS=ON`, app off), `ctest -R wire_sink` passes. One change from the carried patch: 4.6.0
  registers the Wireshark tests inside `if(DECT2_BUILD_APP)`, so a test beside `test_wirecap` is never built headless;
  the branch registers `test_wire_sink` in the headless test block instead (it needs only `dect2core`). The carried
  patch keeps its a63c3a8 placement, which `build.sh` and `test/patch.sh` use. Not yet a pull request: OnAir asks for a
  DCO sign-off, which is the operator's to give.

## 2. Receivers passing sample time to `Hub::publish`

Described only; not written here.

- **Change:** each receiver passes the time of the sample a packet comes from to `Hub::publish`, so the hub stamps
  that time rather than the time of decoding. Today no call site passes a time, so every packet is stamped when it was
  decoded, and a capture of a recording is stamped with the replay's clock, not the recording's.
- **Call sites** (all 16, `Hub::get().publish(` under `core/src`, at OnAir `a63c3a8`):
  - `adsb/adsb_rx.cpp` (1)
  - `ais/ais_rx.cpp` (1, one call per sentence)
  - `asterix/asterix_engine.cpp` (1)
  - `ble/ble_rx.cpp` (1)
  - `cell/cell_rx.cpp` (1, LTE)
  - `dvbs/dvbs_ip.cpp` (1)
  - `gsmr/gsmr_rx.cpp` (1)
  - `halow/halow_rx.cpp` (1)
  - `lorawan/lorawan_rx.cpp` (1)
  - `packet/packet_rx.cpp` (1, APRS)
  - `tetra/tetra_rx.cpp` (2)
  - `wifi/wifi_rx.cpp` (1)
  - `wisun/wisun_rx.cpp` (1)
  - `zwave/zwave_rx.cpp` (1)
  - `zigbee/zigbee_rx.cpp` (1, Zigbee and Thread)
- **Test:** one per receiver touched would be wanted, each feeding a known signal and reading the stamped time. A
  receiver that has no sample clock of its own at the call site needs the count of samples fed so far carried to it.
  Offer it a receiver at a time, starting with ADS-B and APRS, whose call sites are one line each.

## 3. Bus emitters for the voice, broadcast and television receivers

Described only; not written here.

- **Change:** the receivers behind `onair-rx`'s polled modes put their decodes on the automation bus, as the pager,
  ACARS, APRS, VDL2, HFDL and sonde receivers already do inside `feed()`. OnAir defines the families for two of them
  and its core never emits them: `voice.<type>` (`FVoice`, "call events of the digital voice modes") and
  `broadcast.<type>` (`FBroadcast`, "FM RDS / DAB text"). At `a63c3a8` only the app's DAB view emits
  `broadcast.text` and `broadcast.now_playing`, not the FM receiver, and only OnAir's tests make a `voice.call`.
  Television has no family yet; a DVB-T or analog TV lock and its signalling would need one.
- **Receivers:** `FmReceiver` (RDS: a group, or the programme service name and radiotext when complete),
  `DmrReceiver`, `P25Receiver` and `M17Receiver` (a call: its start with the ids, a change, its end), `DvbtReceiver`
  (a lock, a change of the TPS, the service list once read) and `AtvReceiver` (a lock and the system found).
- **Why:** with these events on the bus, `onair-rx`'s generic bus path (`src/bus.cpp`) would serve all six modes
  (`rds`, `dmr`, `p25`, `m17`, `dvbt`, `video`) as table rows. Today each has a session that reads the receiver's
  snapshot at every poll and works out what is new (`src/poll_*.cpp`, `src/calls.h`), which the receiver itself knows
  at the moment it decodes. Any other program taking OnAir's receivers bare would get the same.
- **Test:** one per receiver, feeding the signal of the receiver's own generator and reading the events off a bus
  tap, as `onair-rx`'s `test/poll.sh` reads its lines.
- **No speech switch is asked for:** the voice receivers decode a voice frame only for a call their voice monitor
  plays, and a monitor whose allow list is on and empty refuses every call (`src/poll_voice.cpp`). A program that
  wants the events without the speech therefore already has a way, with no change to OnAir.

## 4. Wi-SUN: the frame's channel and bit rate on the hub, and handing over decoded frames without a report

Described only; not written here.

- **Change:** `WisunReceiver` (`wisun/wisun_rx.cpp`, `accept()`) publishes the MPDU alone (link type 230), so a program
  reading the hub learns neither the channel nor the bit rate of a frame. They could go with it as an IEEE 802.15.4 TAP
  header (link type 283, its channel and PHY fields), as the LoRaWAN receiver's LoRaTap header carries its frequency. And
  `flush()` is the only way to have the decoder thread's frames handed over at a chosen point: it also makes a whole report
  (a copy of every table, sorted), so a caller that wants the frames after every block pays for a report after every block.
  A `takeDecoded()` that waits for the decoder and hands its frames over, without the report, would do.
- **Why:** `onair-rx`'s `wisun` mode reads the channel and the bit rate from the receiver's frame log in its report, and
  flushes after every block so that a frame's line does not depend on when the decoder thread ran (README, `wisun`).
  With `--drop` (no flush per block) a frame can reach the hub before the report that has it, and its line then has no
  channel or bit rate.
- **Test:** the receiver's own test (`tests/wisun/test_wisun_rx.cpp`), reading the header's channel off a hub sink.

## 5. TETRA and GSM-R: an offline switch, and the last stretch

Described only; not written here.

- **Change:** `TetraReceiver` and `GsmrReceiver` (`tetra/tetra_rx.cpp`, `gsmr/gsmr_rx.cpp`, `feed()`) skip the input that
  comes while their worker decodes a stretch; GSM-R also drops the rest of the `feed()` that completes a stretch, even with
  its worker idle. A `setOffline(true)`, as `WisunReceiver` has, would make `feed()` wait for the worker instead and keep
  every sample. GSM-R's stretch (`kBlockSec`, 2.2 s) is not in its header, as TETRA's `kCaptureSec` is. Neither has a
  `flush()` for the input after the last whole stretch: `onair-rx` completes it with zeros.
- **Why:** `onair-rx`'s `tetra` and `gsmr` modes (`src/poll_cell.cpp`) wait on `busy()` after each block (GSM-R: at each
  stretch's end, where they cut the block, with the stretch's length copied from the source), so that a replay faster
  than real time skips nothing and gives the lines of a live run. `onair-rx` reads stdin on a thread of its own so that
  its writer is not held up meanwhile (before that, up to 1.4 s on GSM-R's two-cell scene). A switch inside the receiver
  would let it go on gathering the next stretch while it decodes (TETRA already does), with no wait at all. Padding
  with zeros changes the spectrum of the last stretch the carrier search sees; a `flush()` that decodes what is held as
  it is would not.
- **Test:** the receivers' own tests (`tests/tetra/test_tetra_rx.cpp`, `tests/gsmr/test_gsmr_rx.cpp`), fed without their
  `while (rx.busy()) sleep` pacing.

## 6. LTE: an offline switch, a flush, and a search that can be told less

Described only; not written here.

- **Change:** `CellReceiver` (`cell/cell_rx.cpp`, `feed()`) is GSM-R's case again: it gathers a block (0.2 s, `kBlockSec`,
  not in its header) only while its worker is idle and drops the rest of the `feed()` that completes one. A
  `setOffline(true)` would have `feed()` wait for the worker. There is no `flush()` for the input after the last whole
  block, and padding it with zeros is not neutral here: the cell search tracks the cell over the whole block, so a block
  more than about a tenth zeros loses SIB2 and the SINR, and one under 0.16 s of signal in 0.2 s gives no cell. A
  `flush()` that searches the partial block as it is would keep the tail. And the search costs more than real time at
  every rate on a 12-core host (0.8 s a block at 4.6 Msps, 2.7 s at 20 Msps, on one worker thread), most of it the
  raster probe that `setCenterHz` turns on (every 100 kHz point of the input, then every candidate decoded): a switch
  that names the carriers to look at (a list of EARFCNs, or "the one at the dial frequency"), or the worker spread over
  the candidates, would let a job that knows its cell keep real time.
- **Why:** `onair-rx`'s `lte` mode waits on `busy()` at each block's end, where it cuts the block, so that a replay skips
  nothing; it cannot keep real time without `--drop` (README, `lte`), and a decode is longer than its read-ahead of two
  blocks (no read-ahead helps a receiver slower than its input).
- **Test:** `tests/cell/test_cell_rx.cpp`'s scenes through `CellReceiver` with no pacing, and a 0.1 s input.

## 7. DVB-S: a bound on the symbol rate search, the GPU probe, and the generator in the receiver's link

Described only; not written here.

- **Change:** three small ones in `dvbs/`.
  - A maximum symbol rate for the search (`setMaxSymbolRate`, beside `setSymbolRate`'s manual rate): today the only bound
    is the input rate's (`dvbsMaxSymbolRate`), so `onair-rx --width` filters the input to the band instead (a
    low-pass by overlap-save, 0.14 to 0.18 s of CPU a second at 10 Msps).
  - `ComputeChoice`'s constructor (`dvbs_compute.h`) asks `GpuLdpc::instance().available()` even when the mode is
    set to the CPU afterwards, so making a `DvbsReceiver` sets up Vulkan on the first graphics card and prints three
    lines; `onair-rx` sets `DECT2_NOGPU` to keep the GPU untouched. Asking only when the mode is GPU or auto would do.
  - `rrcAt` (the root raised cosine the receiver's matched filter uses, `dvbs_dsp.cpp`) is defined in `dvbs_gen.cpp`,
    the test signal generator, which also holds `demoTsSource`, the test programme's video encoder: a program linking
    the receiver alone would link the generator and FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libswscale`,
    `libswresample`) and start about 20 MB larger. The wrapper defines `rrcAt` itself (`src/rrc.cpp`, OnAir's body), so
    the generator stays out of its link; moving `rrcAt` into `dvbs_dsp.cpp` upstream would remove that.
- **Already there, no change needed:** `setBlocking(true)` and `flush()` give a replay its deterministic output, and MPE
  and ULE work by handing the receiver's transport stream back through `feedTs` from its packet callback, which runs
  outside the receiver's IP lock.
- **Test:** `tests/dvbs/test_dvbs_gse.cpp` with a bound below the carrier's rate (no lock) and above it (the same lock).

## Dropped

The parent spec also named a patch for OnAir's raw file source to read a pipe. It is not needed: the raw file source
is a plain read, and `onair-rx` feeds the receivers directly.
