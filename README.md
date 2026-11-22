# onair-rx

Runs one receiver of [OnAir](https://github.com/adswill/OnAir) on IQ from stdin: each decode becomes one JSON
line on stdout, and its frames are served as a classic pcap stream on a local TCP port. ZetaSDR runs it as an
external handler (its `onair` handler file), but nothing in it is specific to ZetaSDR.

Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
OnAir contributors: https://github.com/adswill/OnAir/blob/main/CONTRIBUTORS.md

onair-rx is free software under the GNU General Public License, version 3 or later ([LICENSE](LICENSE)). It links
OnAir's libraries, built with the small patches in `patches/` (see Build). OnAir is GPL-3.0-or-later with an
attribution term (its `ADDITIONAL-TERMS.md`), which the two lines above and `onair-rx --version` carry.

## Use

```
onair-rx --mode adsb --rate HZ [--format cf32|cs16] [--pcap-port PORT] < iq
onair-rx --lua > onair.lua     # OnAir's Wireshark Lua dissector (USER0 is Mode S)
onair-rx --version
```

- **stdin:** interleaved little-endian IQ at `--rate` Hz, `cf32` (the default) or `cs16`. ADS-B needs at least
  2 MHz; 2.4 MHz is usual.
- **stdout:** one JSON object per decoded Mode S frame, flushed per line. A line carries only what its own frame
  established, as an SBS (BaseStation) line does, never a value remembered from an earlier frame. A member whose value
  is not known is absent, never null.
  - On every line: `icao` (6 upper-case hex digits), `df` and `level_dbfs` (the preamble pulses' mean power).
  - When the frame has them: `tc` (the type code) and `msg` (its SBS message type, 1 to 8, as OnAir's SBS writer
    classifies it).
  - The members OnAir's SBS writer fills for that message type, from the frame: `callsign` (msg 1), `altitude_ft`
    (msg 3, 5, 7), `ground_speed_kt` and `track_deg` (msg 2, 4), `vertical_rate_fpm` (msg 4), `squawk` (msg 6, four
    octal digits, a string), and `on_ground` when the frame says whether the aircraft is on the ground.
  - `lat` and `lon` only on the position message (msg 2, 3) that completed a fix in OnAir's tracker.
- **`--pcap-port PORT`:** listens on 127.0.0.1:PORT before reading stdin and serves one client at a time a classic
  pcap stream: the file header, then one record per frame (7 or 14 bytes, link type 147, USER0). Every JSON line
  has exactly one record. Records wait in a bounded queue (8192, the oldest dropped first), so a client that
  connects a few seconds late still gets them; a client that hangs up is dropped, and the next one gets a new header.
  Record times are the frame's sample time on the wall clock of the first sample, moved on when the input falls
  more than 100 ms behind real time. What is still queued at the end with no client is counted as dropped.
- **stderr:** diagnostics, one line each. An unknown mode exits 2 with `onair-rx: no mode <m>`; a port that
  cannot be opened, or a failed read of stdin (`onair-rx: stdin: <reason>`), exits 1.
- **EOF** on stdin: what was decoded is written (the queued records to a connected client), then exit 0.
  SIGTERM ends it at once.

## Build

```
ONAIR_SRC=<OnAir checkout> ./build.sh     # the first time: OnAir headless in _onair-build (about 20 minutes)
./build.sh                                # then: only src/ is compiled and linked (seconds)
test/patch.sh                             # the patches apply, _onair-src is current, and the patch's own test passes
test/run.sh                               # needs iq/adsb1090.sigmf-data (cf32, 2457600 Hz); skips the decode checks without it
```

The OnAir checkout is never changed. `build.sh` builds OnAir from `_onair-src`, a copy of `$ONAIR_SRC` with every
`patches/*.patch` applied in name order (`git apply`). The copy is remade when the checkout's commit or a patch's
content changes (its stamp, `_onair-src/.onair-rx-stamp`, holds both hashes). Only the files whose content changed
get a new time, so OnAir rebuilds only what they touch. OnAir then reports its version as the checkout's short commit
followed by `+patched` (`onair-rx --version`: `OnAir a63c3a8+patched`).

- `patches/0001-wirecap-hub-sink.patch`: OnAir's Wireshark hub gains a sink, `Hub::setSink`. The sink sees each
  packet published on an enabled channel, on the publisher's thread, with the time exactly as the caller gave it.
  With the hub's files, live capture and GSMTAP off, it opens no file and no socket. The patch carries an OnAir-style
  test, `tests/wirecap/test_wire_sink.cpp`. OnAir's tests are not built here, so `test/patch.sh` compiles that one
  test against the built libraries and runs it.

`ONAIR_REBUILD=1 ./build.sh` rebuilds OnAir too. Needs the FFmpeg dev packages, libhackrf, libusb-1.0, SoapySDR
(OnAir's server link line), rsync and git.

## How it works

- `src/main.cpp`: the options, the mode table (a mode's name, its pcap link type and the function that runs it),
  the IQ input and the JSON writer. Another OnAir receiver is one more row.
- `src/adsb.cpp`: OnAir's `AdsbReceiver` does the decoding. Its frame callback hands over every good frame, decoded
  by OnAir into `AdsbFrame::msg`; after each `feed()` each frame becomes a line, from the members `feedfmt::sbsLine`
  fills for its message type. A position needs the aircraft's earlier frames, so each address has a small OnAir
  `AdsbTracker` of its own, fed that address's frames; on a position message its one-aircraft snapshot says whether
  this frame committed the fix (OnAir's own feeds get that as `FeedAdsbFrame::hasPos`, but only over sockets). The
  per-frame cost does not grow with the number of aircraft (about 5 us a frame on the test capture). The same
  frame's bytes become the pcap record.
- `src/pcap_port.cpp`: the port, with OnAir's own pcap byte layout (`wire_format.h`).

## Findings from the first spike (2026-11-20, OnAir a63c3a8, Ubuntu 24.04, g++ 13)

- Full headless build: 2418 steps, no errors (FFmpeg dev packages needed; libavdevice optional).
- 8 s of real 1090 MHz IQ from a B210 (cf32, 2.4576 Msps): 207 Mode S frames (143 long, 64 short), 6 aircraft;
  0.67 s of one core (about 12x real time), 8 MB RSS. tshark with `onair.lua` dissects DF, CA, ICAO address, type
  code and parity.
- The receivers share one contract (`configure(rateHz)`, `ready()`, `feed(cf32*, n)`), so the mode table can grow
  to the other channels OnAir's Wireshark hub knows (Wi-Fi, HaLow, Zigbee, Thread, Wi-SUN, BLE, Z-Wave, LoRaWAN,
  DVB IP, AIS, APRS, ASTERIX, GSM-R, TETRA, LTE).
- The spike read frames back from the hub's live stream (`ONAIR-WIRE 1 <mode>` on its local port). The hub stamps
  decode time, not sample time, and needs a loopback socket; taking the bytes from the receiver's frame callback
  instead gives one source for the JSON lines and the records.
- OnAir's whole engine has no stdin source as is (its SigMF, WAV and BLUE readers size, seek and loop the file);
  modes that need the engine's own chain rather than a bare receiver need a stdin `IqSource`.
