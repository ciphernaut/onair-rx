# What to offer upstream

Two changes to OnAir, as the operator would send them. Nothing here has been sent. OnAir takes focused pull requests
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
- **Before it is offered:** the patch's `CMakeLists.txt` hunk has never been configured with `DECT2_BUILD_TESTS=ON`.
  The test has only been compiled by hand by `test/patch.sh`. Build it once in OnAir's own test build, and fix the
  hunk if that build needs it.

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

## Dropped

The parent spec also named a patch for OnAir's raw file source to read a pipe. It is not needed: the raw file source
is a plain read, and `onair-rx` feeds the receivers directly.
