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

Modes: `adsb`, the bus modes `pocsag`, `flex`, `acars`, `aprs`, `ais`, `vdl2`, `hfdl`, `rs41`, `dfm` and `m20`, and
the polled modes `rds`, `dmr`, `p25` and `m17` (below).

- **stdin:** interleaved little-endian IQ at `--rate` Hz, `cf32` (the default) or `cs16`, the signal's channel at
  0 Hz (HFDL: its dial frequency at 0 Hz). The receivers' minimum rates: ADS-B 2 MHz (2.4 MHz is usual); the bus
  and polled modes' are in their tables below. A rate below the mode's exits 2 with one line saying so.
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
- **stdout, polled modes:** some OnAir receivers put nothing on the bus and offer only a snapshot of their state
  (`telemetry()`). A polled mode feeds its receiver blocks of 16384 samples, each one whole however the input arrives
  (only the last may be short), and polls at the multiples of the mode's poll interval from sample 0, each after the
  block that reaches one, so a replay gives the same lines as a live run, byte for byte. A poll writes one JSON object
  per line for what is new or has changed since the last poll; one more poll runs at the end of input, so that what
  the last block completed is written. Every line has `event` (the mode's) and `t_sample` (the index of the first
  sample of the block after which the poll ran), a member only when its value is known, whole numbers without a
  decimal point. Text the receiver hands over in UTF-8 (`ps`, `rt`, `alias`) is written as UTF-8, escaping only a
  quote, a backslash and a control character; text that is not valid UTF-8 is written byte by byte as above, so a line
  is always valid JSON. None of them has packets: `--pcap-port` exits 2 with
  `onair-rx: <mode> has no packets (no --pcap-port)`, before reading any input. The modes:

  | mode | OnAir receiver | minimum rate | poll | event | members |
  |---|---|---|---|---|---|
  | `rds` | `FmReceiver` | 500 kHz | every block | `rds.group` | `pi`, `group`, `pty`, `tp`, `ps`, `rt` |
  | `dmr` | `DmrReceiver` (`configureChannel`) | 48 kHz | 0.25 s | `voice.call` | a call: `type`, `src`, `dst`, `call`, `color_code`, `slot`, `encrypted`, `emergency`, `silent`, `alias`, `eot`; a channel: `type`, `color_code`, `control`, `network` |
  | `p25` | `P25Receiver` (phase 1) | 250 kHz | 0.25 s | `voice.call` | a call: `type`, `src`, `dst`, `call`, `nac`, `encrypted`, `emergency`, `silent`, `algid`, `kid`, `eot`; a channel: `type`, `nac`, `control`, `network` |
  | `m17` | `M17Receiver` | 250 kHz | 0.25 s | `voice.call` | `src`, `dst`, `can`, `type`, `encryption`, `lat`, `lon`, `eot` |

  - `rds`: the FM station at 0 Hz of the input (OnAir's station finder is off, so the station is the one at the
    centre however weak; the receiver is silent, no sound device is opened). One line per RDS group from the
    receiver's group tap whose block B and PI (block A, or C' in a version B group) came with a right checkword: a
    block the receiver corrected is not used, since OnAir trusts one only once it repeats and the tap hands it over
    before that. From the group: `pi` (four upper-case hex digits), `group` (its type and version, `0A`, `2B`), `pty`
    (the programme type, 0 to 31) and `tp` (the traffic programme flag). From OnAir's snapshot at that group (published
    every 0.1 s of signal): `ps`, the programme service name, once all its segments came, and `rt`, the radiotext,
    once it is complete; each is absent until then. Both are UTF-8, read by OnAir from the station's character set
    (EBU Latin, or GB2312 when the bytes pair up as it): a station's `Rádio` is `Rádio`.
  - `dmr`, `p25`, `m17` (`voice.call`): the channel at 0 Hz of the input, a call per line as the receiver's call log
    has it (its snapshot, published every 0.25 s of signal, holds the calls that ended, the oldest aged out, and the one
    going on). A call gives one line when it is first seen, one when a member of it changes (a talker alias that comes
    late, the encryption flag) and one when it ends (`eot` true; false on the others): when the log marks it over, or,
    if it ages out of the log first, when it is gone; the last poll at EOF ends a call still open. A call that stays up
    gives no line per poll, and an ended call that stays in the log gives none. A call is keyed by where it was heard
    (DMR: its time slot) and when it started, so the same call sent again (a capture played in a loop) is a new call
    with its own first and last line. Its first line waits until the source, the destination and the colour code (DMR)
    or NAC (P25) are in hand (M17: the source, the destination and the type), so the first line of a call is complete;
    a call whose ids never come gives no line. The values are spelled as ZetaSDR's rows spell them:
    - `dmr`: `type` `call`, `src` and `dst` (radio and talkgroup ids), `call` (`group`, also for an all call, or
      `private`), `color_code`, `slot` (1 or 2, when the receiver could tell), `encrypted` (the privacy flag),
      `emergency`, `alias` (the talker alias, when OnAir has it). The input may be any rate from 48 kHz: the receiver
      is set up as for a channel already cut out of a wider capture (`configureChannel`), not with its radio minimum of
      1 MHz.
    - `p25`: `type` `call`, `src`, `dst`, `call`, `nac`, `encrypted`, `emergency`, and `algid` and `kid` (the
      algorithm and key ids) once the call's header gave them, or an LDU2 changed them: a call joined late has neither
      until then. The manufacturer id (`mfid`) is not in OnAir's call log, so it is never written. Voice grants and data
      packets in the log are not calls of the channel and give no line.
    - `m17`: `src` and `dst` (callsigns; the broadcast address, OnAir's `@ALL`, is `ALL`, and an extended-range
      address is `#` and the base-40 digits of its offset, as ZetaSDR spells them), `can`, `type` (`voice`,
      `voice_data`, `data`, `packet`; a reserved data type is `data`), `encryption` (`scrambler`, `aes`, `other`;
      absent when clear), and `lat`, `lon` when the transmission carries a position. A BERT stream gives no line.
    - `silent` is `encrypted` on an encrypted DMR or P25 call and absent otherwise: no speech is decoded here at all.
      The receivers are silent (no sound device), and each has a voice monitor of the wrapper's own whose allow list is
      on and empty, so it refuses every call: OnAir decodes a voice frame only for a call its monitor plays, so no
      frame goes through mbelib or Codec 2 whether they are installed or not, and the lines are the same either way. To
      check that by hand, hide the libraries from OnAir with `DECT2_MBELIB_PATH=/nonexistent` and
      `DECT2_CODEC2_PATH=/nonexistent` (OnAir then looks for no other copy).
    - A channel line (`dmr`, `p25`: `type` `channel`, `color_code` or `nac`, `control`, and `network`, the text
      ZetaSDR's row shows for it, `channel, colour code 5` or `control channel, NAC 291`) says the channel is on the air
      with no call. The colour code and the NAC stay in OnAir's snapshot after the carrier is gone, so they alone do not
      say it; what does: a DMR time slot's latest burst, within the last second, idle or control blocks, on every slot
      that has bursts (a repeater's: both) and no call going on; a P25 control channel's blocks within the last 3 s
      (a conventional channel sends nothing between calls, so it gives no channel line). It is written when it first
      holds, and again only when what it says changes, not after every call.
- **`--pcap-port PORT`:** listens on 127.0.0.1:PORT before reading stdin and serves one client at a time a classic
  pcap stream: the file header, then one record per frame. ADS-B: 7 or 14 bytes, link type 147 (USER0), and every
  JSON line has exactly one record. APRS: the AX.25 frame as OnAir's Wireshark hub gets it (link type 3), also a
  digipeater's copy, which has no line. AIS: each sentence with its line end in IPv4 + UDP to port 10110 (NMEA 0183),
  link type 101 (raw IP), as OnAir's capture files have it. The other bus modes and the polled modes have no packets
  and refuse the option (exit 2). Records wait in a bounded queue (8192, the oldest dropped first), so a client that
  connects a few seconds late still gets them; a client that hangs up is dropped, and the next one gets a new header.
  Record times are the frame's sample time (bus modes: its block's first sample) on the wall clock of the first sample,
  moved on when the input falls more than 100 ms behind real time. What is still queued at the end with no client is
  counted as dropped. The decoding never waits for the port.
- **stderr:** diagnostics, one line each. An unknown mode exits 2 with `onair-rx: no mode <m>`; a port that
  cannot be opened, or a failed read of stdin (`onair-rx: stdin: <reason>`), exits 1.
- **EOF** on stdin: what was decoded is written (the queued records to a connected client), then exit 0.
  There is no flush at the end of input: a decode a receiver still holds when stdin closes is not written. A short
  replay (ZetaSDR's `capture_decode`) can lose its last message this way. A polled mode polls once more at EOF, so
  what its receiver's snapshot has by then is written.
  SIGTERM ends it at once.

## Build

```
ONAIR_SRC=<OnAir checkout> ./build.sh     # the first time: OnAir headless in _onair-build (about 20 minutes)
./build.sh                                # then: only src/ is compiled and linked (seconds)
test/patch.sh                             # the patches apply, _onair-src is current, and the patch's own test passes
test/run.sh                               # needs iq/adsb1090.sigmf-data (cf32, 2457600 Hz); skips the decode checks without it
test/bus.sh                               # the bus modes on OnAir's own test signals (_build/onair-gen)
test/poll.sh                              # the polled modes on OnAir's own test signals (_build/onair-gen), the voice
                                          # modes also with mbelib and Codec 2 hidden
```

The wrapper's headers are on the quoted include path only (`-iquote`), so that `src/poll.h` does not stand in for the
system's `<poll.h>`.

`build.sh` also builds `_build/onair-gen` (`test/gen.cpp`): `onair-gen --mode
pocsag|flex|acars|aprs|ais|vdl2|hfdl|rs41|dfm|m20 --rate HZ [--freq HZ] [--text TEXT] [--info INFO]` writes cf32 IQ of
one known message from OnAir's test-signal generator for the mode (AIS: a position report on AIS 1 and a static report
on AIS 2; VDL2: a downlink, an uplink and a ground station's GSIF; HFDL: a squitter and a logon request; a sonde: 12 s
of one sonde of OnAir's scene, moved to 0 Hz; `--info` replaces APRS's whole information field), with a second of
noise either side, and says on stderr where the message is. `onair-gen --mode rds --rate HZ [--ps PS] [--rt RT]` is a
station instead, with nothing around it: 6 s of OnAir's `FmGenerator` with PI `2AB1`, PS `ZETA FM ` and RT
`HELLO FROM ZETASDR` (or the bytes of the RDS character set given), with the generator's own noise.
`onair-gen --mode dmr|p25|m17 --rate HZ [--channel]` is one call with the values of ZetaSDR's own checks, sent twice
with 2 s of noise between, as a check's capture repeats: DMR, a handheld's group call in direct mode on slot 1, colour
code 5, from 9990001 to talkgroup 9990, three superframes; P25, a group call of NAC 0x123 from 9990001 to talkgroup
65000; M17, a voice stream from `VK4ZET` to `ALL` on CAN 0, 25 frames. `--channel` is a channel instead (DMR: 4 s of a
repeater's downlink, its traffic quiet; P25: 4 s of a control channel of the same NAC). An option the mode does not
take is refused with the usage line. `onair-gen --selftest ais-join` tests the AIS sentence joiner of
`src/ais.cpp`, which it links, `onair-gen --selftest sonde-id` the spelling of a sonde's `id` (`sondeId`, `src/bus.h`)
on serials the test signals do not have, `onair-gen --selftest json-utf8` the UTF-8 text writer (`Json::utf8`,
`src/json.cpp`) on valid and broken UTF-8, and `onair-gen --selftest calls` the call keying of the voice modes
(`CallLog`, `src/calls.h`) on snapshots it makes.

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
- `src/poll.cpp`: the polled modes. A mode is a row of `kPollModes`: its name, its event, its poll interval and the
  function that makes its session (`src/poll_<mode>.cpp`), a `PollSession` that feeds the receiver and, at each poll,
  hands over the lines for what is new or changed in its snapshot; `runPolled` adds `event` and `t_sample`. RDS
  (`src/poll_rds.cpp`) queues a line per group from the receiver's group tap and is polled after every block. The
  voice modes (`src/poll_voice.cpp`) are polled every 0.25 s: each turns its receiver's call log into what it says
  about each call (a key and the members) and hands that to a `CallLog` (`src/calls.h`), which writes the first, the
  changed and the last line of each call.
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
