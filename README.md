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
onair-rx --mode MODE --rate HZ [--format cf32|cs16] [--freq HZ] [--pcap-port PORT] < iq
onair-rx --lua > onair.lua     # OnAir's Wireshark Lua dissector (USER0 is Mode S)
onair-rx --version
```

Modes: `adsb`, and the bus modes `pocsag`, `flex`, `acars`, `aprs`, `ais`, `vdl2`, `hfdl`, `rs41`, `dfm` and `m20`
(below).

- **stdin:** interleaved little-endian IQ at `--rate` Hz, `cf32` (the default) or `cs16`, the signal's channel at
  0 Hz (HFDL: its dial frequency at 0 Hz). The receivers' minimum rates: ADS-B 2 MHz (2.4 MHz is usual); the bus
  modes' are in the table below. A rate below the mode's exits 2 with one line saying so.
- **`--freq HZ`:** the radio frequency at 0 Hz of the input. Every mode takes it; ACARS, AIS, VDL2 and HFDL need it
  (they place their channels by frequency) and exit 2 with `onair-rx: <mode> needs --freq HZ` without it, before
  reading any input.
  AIS also exits 2, before reading input, with `onair-rx: ais: no AIS channel within <rate> Hz of <freq> Hz` when the
  input does not hold AIS 1 (161.975 MHz) or AIS 2 (162.025 MHz) as OnAir's receiver needs them: the middle of the two,
  162.000 MHz, 40 kHz inside the edge of the input's band (`--freq` plus or minus 0.49 times `--rate`).
- **stdout, adsb:** one JSON object per decoded Mode S frame, flushed per line. A line carries only what its own frame
  established, as an SBS (BaseStation) line does, never a value remembered from an earlier frame. A member whose value
  is not known is absent, never null.
  - On every line: `icao` (6 upper-case hex digits), `df` and `level_dbfs` (the preamble pulses' mean power).
  - When the frame has them: `tc` (the type code) and `msg` (its SBS message type, 1 to 8, as OnAir's SBS writer
    classifies it).
  - The members OnAir's SBS writer fills for that message type, from the frame: `callsign` (msg 1), `altitude_ft`
    (msg 3, 5, 7), `ground_speed_kt` and `track_deg` (msg 2, 4), `vertical_rate_fpm` (msg 4), `squawk` (msg 6, four
    octal digits, a string), and `on_ground` when the frame says whether the aircraft is on the ground.
  - `lat` and `lon` only on the position message (msg 2, 3) that completed a fix in OnAir's tracker.
- **stdout, bus modes:** one JSON object per event OnAir's receiver puts on its automation bus, flushed per line:
  the event's fields as members (whole numbers without a decimal point, a non-finite number left out), plus `event`
  (the bus type) and `t_sample` (the index of the first sample of the block of 16384 whose decoding produced it).
  Strings are written byte by byte: a byte above 0x7e, 0x7f and a control character as `\u00XX`. A byte above 0x7e
  therefore arrives as the Latin-1 character of that value, so a UTF-8 comment shows as two or three characters per
  character it holds. The modes:

  | mode | OnAir receiver | minimum rate | `--freq` | bus event (its fields) | what onair-rx adds |
  |---|---|---|---|---|---|
  | `pocsag` | `PagerReceiver` (POCSAG) | 1 MHz | no | `pager.message` (`protocol`, `speed`, `address`, `function`, `type`, `text`, `damaged`) | `baud`; POCSAG pages only |
  | `flex` | `PagerReceiver` (FLEX) | 1 MHz | no | `pager.message` (the same) | `baud`, `levels`; FLEX pages only |
  | `acars` | `AcarsReceiver` | 1 MHz | yes: the channel | `acars.message` (`tail`, `flight`, `label`, `text`, `downlink`, `msg_num`, `lat`, `lon`) | nothing |
  | `aprs` | `PacketReceiver` | 240 kHz | no | `aprs.packet` (`from`, `to`, `info`, `type`, `summary`, `lat`, `lon`) | `path`, `fcs_ok`; no `lat`, `lon` for an object, item or third-party packet |
  | `ais` | `AisReceiver` | 250 kHz | yes: AIS 1 or 2 inside the band | none: `ais.message` from the hub's sentences | the whole line |
  | `vdl2` | `Vdl2Receiver` | 250 kHz | yes: the channel | `vdl2.message` (`src`, `dst`, `from_air`, `proto`, `summary`, `frame`) | `icao`, `addr_type` |
  | `hfdl` | `HfdlReceiver` | 24 kHz | yes: the dial frequency | `hfdl.message` (`kind`, `summary`, `icao`, `uplink`, `snr_db`) | `icao` left out when the bus has none |
  | `rs41`, `dfm`, `m20` | `SondeReceiver` | 2 MHz (1 MHz with an L-band `--freq`) | no (taken) | `sonde.update` (`serial`, `type`, `lat`, `lon`, `alt_m`, `vspeed_ms`, `hspeed_ms`, `temp_c`, `batt_v`, `sats`) | `id` (the serial as rs1729 spells it); that type's sondes only |

  Every mode writes only the event of its row: any other event its receiver puts on the bus is dropped. In more
  detail:
  - `pocsag`, `flex` (`pager.message`: `protocol`, `speed`, `address`, `function`, `type`, `text`, `damaged`):
    `baud` from the speed, and for FLEX `levels` (2 or 4). Each mode decodes its own protocol only.
  - `acars` (`acars.message`: `tail`, `flight`, `label`, `text`, `downlink`, `msg_num`, `lat`, `lon`): nothing; the
    receiver watches the one channel at `--freq`.
  - `aprs` (`aprs.packet`: `from`, `to`, `info`, `type`, `summary`, `lat`, `lon`): `path` as an
    array of strings, and `fcs_ok`: OnAir passes on only frames whose check sequence was right or that it repaired
    until it was, and marks a repaired frame in its summary; `fcs_ok` is false for those. `lat` and `lon` are left out
    for an object or an item (`type` `Object` or `Item`: the position is the object's) and for a third-party packet
    (`info` starting with `}`: the position is the station's inside it), so that a line's position is always the
    sender's, as OnAir keeps it. An APRS message is its `aprs.packet` line only: the `aprs.message` OnAir's bus also has
    for it is dropped.
  - `ais`: one line per AIS message, made from the NMEA 0183 sentences OnAir's receiver hands its Wireshark hub
    (`src/ais.cpp`), not from the bus: every line is an `ais.message`, and the receiver's own bus events (the station
    summaries `ais.vessel`, and `ais.anomaly`, `ais.behaviour`, `ais.cpa`, `ais.geofence`, which OnAir makes of the
    messages over time) are all dropped. A message's line has `event` `ais.message`, `mmsi`,
    `msg_type`, `channel` (`A` for AIS 1, `B` for AIS 2), `repeat` and `nmea` (the message's sentences, without line
    ends), and `lat`, `lon`, `sog_kt`, `cog_deg`, `heading_deg`, `nav_status`, `name`,
    `callsign`, `destination`, `imo` and `ship_type` when the message type carries them and does not give them as not
    available (navigational status 15, IMO number 0 and ship type 0 are). The message is read by OnAir's own AIS
    parser. A message of several sentences becomes one line when its last sentence comes; a part that does not follow
    the one before it (by channel and sequence id) ends that message without a line, and a channel's complete message
    forgets the channel's parts still waiting. The receiver decodes AIS 1 and 2, whichever of them the input holds
    (one is enough, so `--freq` may be either channel's own frequency), not the long-range channels 75 and 76.
  - `vdl2`: the receiver decodes the one channel at `--freq` (OnAir's default list, the European set, is not used),
    a 25 kHz channel at 0 Hz of the input. `icao` is the frame's source address, six upper-case hex digits, and
    `addr_type` the source's type, as dumpvdl2 has them: the aircraft's address and `Aircraft` on a downlink
    (`from_air`), the ground station's address and `Ground station` on an uplink or a broadcast (the bus carries no
    address types; `from_air` is the source being an aircraft). `dst` is the destination as the bus has it
    (`FFFFFF` for all stations, as a GSIF has it). `frame` is the AVLC frame (`I`, `RR`,
    `XID`, `GSIF` ...), `proto` what it carries (`ACARS`, `X.25`, `CLNP`, `XID`, `COTP`, or empty).
  - `hfdl`: `--freq` is the channel's dial frequency, the one HFDL's frequency lists give, and the input is centred
    on it: the receiver takes the carrier 1440 Hz above the dial frequency (OnAir's `kHfdlCarrierHz`), as a USB
    receiver tuned to the listed frequency hears it, so a band of 6 kHz around `--freq` holds it. The one channel at
    `--freq` is decoded, not OnAir's station table. `icao` is absent on a frame that names no aircraft (a squitter);
    `kind` is the PDU (`SPDU` for a squitter, `Logon request` ...).
  - `rs41`, `dfm`, `m20`: the receiver finds the sondes in the band by their carriers, opens a channel on each and
    tells the type from the frame sync, so the sonde may sit anywhere in the band, at 0 Hz too (as a 12.5 kHz channel
    resampled up to 2 MHz has it); nothing is set but the rate (and the centre, with `--freq`, for the bus's
    frequency). The minimum rate is OnAir's: 2 MHz, or 1 MHz when `--freq` is in the L band (OnAir's
    `sondeIsLbandMhz`, the LMS6-1680 profile); without `--freq` it is 2 MHz. Each mode keeps the events of its own
    type (`type` `RS41`, `DFM` or `M20`) and drops the others'. An event comes with a frame that has a position, at
    most every 5 s per sonde (OnAir's own pace); one before the sonde's serial is known is dropped, so that every line
    has an `id`. Since the next event comes 5 s later, a DFM, whose serial OnAir puts together over several frames,
    may get its first line up to 5 s after its first position. `serial` is OnAir's; `id` is the serial as rs1729's
    decoder for the type writes its `id` (so as ZetaSDR's rows for those decoders key their subjects):
    - `rs41`: the serial (`S1234567`, as rs41mod has it);
    - `m20`: `M20-` and the serial (`M20-611-2-01234`, as m20mod has it; OnAir's serial is `611-2-01234`);
    - `dfm`: `DFM-` and the number as dfm09mod writes it: `%6u` (DFM-09/17) or `%6X` (DFM-06), so space-padded to
      six characters, and at most nine (`DFM-17012345`; a DFM-09's `12345` and a DFM-06's `012345` are `DFM- 12345`).
    The receiver runs inside `feed()` (OnAir's synchronous mode), so that nothing is dropped when the input comes
    faster than real time.

  Not on the generic path: none of the modes so far. A receiver that needs more than a channel, a centre and a type
  filter (a second channel, an engine of its own) gets a line here saying what it needed.
- **`--pcap-port PORT`:** listens on 127.0.0.1:PORT before reading stdin and serves one client at a time a classic
  pcap stream: the file header, then one record per frame. ADS-B: 7 or 14 bytes, link type 147 (USER0), and every
  JSON line has exactly one record. APRS: the AX.25 frame as OnAir's Wireshark hub gets it (link type 3), also a
  digipeater's copy, which has no line. AIS: each sentence with its line end in IPv4 + UDP to port 10110 (NMEA 0183),
  link type 101 (raw IP), as OnAir's capture files have it. The other bus modes have no packets and refuse the option
  (exit 2). Records wait in a bounded queue (8192, the oldest dropped first), so a client that connects a few seconds
  late still gets them; a client that hangs up is dropped, and the next one gets a new header.
  Record times are the frame's sample time (bus modes: its block's first sample) on the wall clock of the first sample,
  moved on when the input falls more than 100 ms behind real time. What is still queued at the end with no client is
  counted as dropped. The decoding never waits for the port.
- **stderr:** diagnostics, one line each. An unknown mode exits 2 with `onair-rx: no mode <m>`; a port that
  cannot be opened, or a failed read of stdin (`onair-rx: stdin: <reason>`), exits 1.
- **EOF** on stdin: what was decoded is written (the queued records to a connected client), then exit 0.
  There is no flush at the end of input: a decode a receiver still holds when stdin closes is not written. A short
  replay (ZetaSDR's `capture_decode`) can lose its last message this way.
  SIGTERM ends it at once.

## Build

```
ONAIR_SRC=<OnAir checkout> ./build.sh     # the first time: OnAir headless in _onair-build (about 20 minutes)
./build.sh                                # then: only src/ is compiled and linked (seconds)
test/patch.sh                             # the patches apply, _onair-src is current, and the patch's own test passes
test/run.sh                               # needs iq/adsb1090.sigmf-data (cf32, 2457600 Hz); skips the decode checks without it
test/bus.sh                               # the bus modes on OnAir's own test signals (_build/onair-gen)
```

`build.sh` also builds `_build/onair-gen` (`test/gen.cpp`): `onair-gen --mode
pocsag|flex|acars|aprs|ais|vdl2|hfdl|rs41|dfm|m20 --rate HZ [--freq HZ] [--text TEXT] [--info INFO]` writes cf32 IQ of one known
message from OnAir's test-signal generator for the mode (AIS: a position report on AIS 1 and a static report on AIS 2;
VDL2: a downlink, an uplink and a ground station's GSIF; HFDL: a squitter and a logon request; a sonde: 12 s of one
sonde of OnAir's scene, moved to 0 Hz; `--info` replaces APRS's whole information field), with a second of noise either side, and says on stderr where the message is. `onair-gen --selftest ais-join` tests the AIS sentence joiner of `src/ais.cpp`, which it links, and `onair-gen
--selftest sonde-id` the spelling of a sonde's `id` (`sondeId`, `src/bus.h`) on serials the test signals do not have.

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

- `src/main.cpp`: the options, the table of modes with a run function of their own (a mode's name, its pcap link
  type and the function that runs it) and the IQ input. `src/json.cpp`: the JSON writer.
- `src/bus.cpp`: the bus modes. OnAir's receivers put each decoded message on OnAir's automation bus and each frame
  on OnAir's Wireshark hub, both inside `feed()` on the calling thread, with no engine started. One run serves them
  all: a tap on the bus and a sink on the hub (`patches/0001`) collect what a block's `feed()` produced, then the
  events become lines and the packets records. A mode is a row of `kBusModes`: how to make and set up its receiver,
  its hub channel, whether it needs `--freq`, and what its lines need that the bus lacks. A mode whose lines come
  from its hub packets (AIS) names the lines' event and a function that turns a packet into a line; its hub channel
  then runs with or without a pcap port. Another receiver on the bus is one more row.
- `src/ais.cpp`: AIS's lines. A joiner puts a message's sentences back together (OnAir publishes them one after the
  other); OnAir's `ais::parseNmea`, `ais::unarmour` and `ais::decodeMessage` read the message.
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
