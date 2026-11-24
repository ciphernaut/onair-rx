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
onair-rx --mode MODE --rate HZ [--format cf32|cs16] [--freq HZ] [--pcap-port PORT] [--bandwidth MHZ]
         [--width HZ] [--plan NAME] [--region NAME] [--drop] < iq
onair-rx --lua > onair.lua     # OnAir's Wireshark Lua dissector (USER0 is Mode S)
onair-rx --version
```

Modes: `adsb`, the bus modes `pocsag`, `flex`, `acars`, `aprs`, `ais`, `vdl2`, `hfdl`, `rs41`, `dfm`, `m20`, `ble`,
`lorawan`, `zigbee`, `thread`, `zwave`, `wifi` and `wisun`, and the polled modes `rds`, `dmr`, `p25`, `m17`, `dvbt`,
`video`, `tetra` and `gsmr` (below). `ble`, `lorawan`, `zigbee`, `thread`, `zwave`, `wifi`, `wisun`, `tetra` and `gsmr` are
modes ZetaSDR has no decoder of its own for: their point is their packets, which Wireshark reads.

- **stdin:** interleaved little-endian IQ at `--rate` Hz, `cf32` (the default) or `cs16`, the signal's channel at
  0 Hz (HFDL: its dial frequency at 0 Hz). The receivers' minimum rates: ADS-B 2 MHz (2.4 MHz is usual); the bus
  and polled modes' are in their tables below. A rate below the mode's exits 2 with one line saying so.
- **`--freq HZ`:** the radio frequency at 0 Hz of the input. Every mode takes it; ACARS, AIS, VDL2 and HFDL need it
  (they place their channels by frequency), as do `lorawan`, `zigbee`, `thread`, `zwave`, `wifi`, `wisun`, `tetra` and
  `gsmr`; they exit 2 with
  `onair-rx: <mode> needs --freq HZ` without it, before reading any input.
  AIS also exits 2, before reading input, with `onair-rx: ais: no AIS channel within <rate> Hz of <freq> Hz` when the
  input does not hold AIS 1 (161.975 MHz) or AIS 2 (162.025 MHz) as OnAir's receiver needs them: the middle of the two,
  162.000 MHz, 40 kHz inside the edge of the input's band (`--freq` plus or minus 0.49 times `--rate`).
- **`--bandwidth MHZ`:** `dvbt` only: the channel width, 8, 7, 6 or 5 MHz; without it each is tried in turn (below).
  Another value exits 2 with `onair-rx: bad --bandwidth <v> (8, 7, 6 or 5)`, and the option with another mode with
  `onair-rx: <mode> has no --bandwidth (dvbt only)`, before reading any input.
- **`--width HZ`, `--plan NAME`, `--region NAME`, `--drop`:** options only some modes take (`kOnlySome`, `src/main.cpp`);
  with any other mode each exits 2, before reading any input, with `onair-rx: <mode> takes no --<option>`. A `--width`
  that is not a number above 0 exits 2 with `onair-rx: bad --width <v>`. Today:
  - `--plan` (`lorawan`): the LoRaWAN regional plan, one of OnAir's `EU868`, `US915`, `AU915`, `AS923-1`, `AS923-2`,
    `AS923-3`, `AS923-4`, `EU433` (in any case). Without it the plan is found (below).
  - `--region` (`zwave`): the Z-Wave region, one of OnAir's `EU`, `US`, `ANZ`, `HK`, `IN`, `RU`, `CN`, `IL`, `JP`, `KR`
    (in any case). Without it the region is found (below).
  - `--region` (`wisun`): the Wi-SUN band plan, one of OnAir's `ANZ`, `NA`, `MX`, `BZ`, `JP`, `KR`, `PH`, `MY`, `HK`,
    `SG`, `SG866`, `TH`, `VN`, `EU863`, `EU870`, `IN`, `CN470`, `CN779`, `CN920`, `WW` (in any case). Without it the
    receiver takes the plan for `--freq` (below).
  - `--drop` (`wisun`, `tetra`, `gsmr`): a receiver with a worker skips the input that comes while it is busy, as it does
    live, instead of being waited for (below).
  - `--width` (the job's band, Hz) is taken by no mode yet: every mode refuses it.
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
  | `ble` | `BleReceiver` | 4 MHz | no (taken: the channel tried first) | `ble.device` (`mac`, `name`, `rssi_db`) | `mac` lower-case without separators; `name` left out when empty |
  | `lorawan` | `LorawanReceiver` | 1 MHz | yes: places the plan's channels | `lora.packet` (`kind`, `hex`, `snr_db`, `sf`, `modulation`, `devaddr`) | `freq_hz`; `devaddr` lower-case |
  | `zigbee` | `ZigbeeReceiver` | 4 MHz | yes: the PHY and the channel | none: `zigbee.frame` from the hub's PSDUs | the whole line |
  | `thread` | `ZigbeeReceiver`, Thread mode | 4 MHz | yes: the channel | none: `thread.frame` from the hub's PSDUs | the whole line |
  | `zwave` | `ZwaveReceiver` | 1 MHz | yes: places the region's channels | none: `zwave.frame` from the hub's MPDUs | the whole line |
  | `wifi` | `WifiReceiver` | 20 MHz | yes: the band | none: `wifi.frame` from the hub's frames | the whole line |
  | `wisun` | `WisunReceiver` | 2 MHz | yes: places the band plan's channels | none: `wisun.frame` from the hub's MPDUs | the whole line |

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
  - `ble`: OnAir's Bluetooth LE advertising monitor (LE 1M PHY) on the one advertising channel at 0 Hz of the input: it
    dewhitens each packet for channels 37, 38 and 39 in turn and keeps the one whose CRC passes, so it needs no channel
    setting; `--freq`, when given, only says which channel it tries first. Advertising only: connections are not followed
    (a `CONNECT_IND` is a line of its sender, nothing more), nothing is decrypted. A line per good packet that carries an
    address: the advertiser's, or the scanner's for a scan or connection request (an extended advertisement, which
    names none, has a packet and no line). `mac` is the address in lower-case hex without separators (OnAir's
    `C0:FF:EE:12:34:56` is `c0ffee123456`), `name` the packet's own name (absent when it has none: a device's name comes
    in some of its packets only), `rssi_db` the packet's level in dB below full scale (not calibrated). OnAir's minimum
    rate, 4 MHz, is its own decimation to 4 samples a bit.
  - `lorawan`: OnAir's LoRaWAN monitor on every channel of one regional plan inside the input, at the spreading factors
    the plan's data rates use, the gateways' inverted-IQ frames too, and the plan's FSK channel (EU868 868.8 MHz, AS923-1
    924.8 MHz). OnAir's other settings are its defaults: every sub-band, the downlinks, at most 24 channels (those nearest
    the centre: a wide US915 or AU915 input has more). Nothing is decrypted and no MIC is checked: frames are given as
    received. The plan: `--plan` when given; otherwise the plan of OnAir's table (in its order: EU868, US915, AU915,
    AS923-1 to 4, EU433) with the most channels inside the input around `--freq`, a tie going to the earlier, where a
    channel is inside as OnAir's own channel bank has it (`lorabank::Bank::fits`: its edge plus 25 kHz within 0.45 of the
    rate from the centre). With no plan having a channel inside, it exits 2, before reading input, with
    `onair-rx: lorawan: no channel plan has a channel within <lo>-<hi> MHz` (`--freq` plus or minus half the rate); a
    `--plan` with no channel inside with `onair-rx: lorawan: plan <plan> has no channel within <lo>-<hi> MHz`, and an
    unknown one with `onair-rx: lorawan: no plan <name> (EU868, US915, ...)`. A line per frame whose CRC passed, or that has
    none (a downlink) and that OnAir reads as LoRaWAN: `kind` is always `lorawan`, `hex` the PHY payload as OnAir's bus has it
    (upper-case byte pairs, a space between), `snr_db` in the LoRa bandwidth, `sf` the spreading factor (0 for an FSK
    frame), `modulation` `lora` or `fsk`, `devaddr` (eight lower-case hex digits) on a data frame, and `freq_hz` the
    channel's centre in Hz, from the bus event's frequency. A join request has no `devaddr`: its EUIs are in `hex`.
  - `zigbee`, `thread`, `zwave`, `wifi`, `wisun`: OnAir's receivers for 802.15.4 (Zigbee, and Thread, which is the same
    receiver in `setThreadMode(true)`), Z-Wave, Wi-Fi and Wi-SUN put nothing on the bus and hand each frame whose check
    passed to the Wireshark hub, from inside `feed()` (Wi-SUN: inside `feed()` or `flush()`, below). A line is what OnAir's own parser reads from that packet (`src/frames.cpp`, one function
    for each mode, a `BusMode`'s `packetLines`); a packet the parser cannot read, or that is not what the mode's line is
    about, gives no line and is still a pcap record. Nothing is decrypted and no key is loaded: frames are given as
    received. Every member is there only when the frame has it. Addresses are lower-case hex without separators.
    - `zigbee` (`zigbee.frame`, `zigbee::parse`): `frame_type` (`beacon`, `data`, `ack`, `command`; OnAir's other names for
      the 2015 types, `multipurpose`, `fragment` and `extended`; a reserved type has no line), `seq`, `pan` (4 hex digits:
      the destination's PAN, else the source's), `src` and `dst` (4 hex digits for a short address, 16 for an extended one;
      none for a frame that has none, such as an ack), `nwk_src` and `nwk_dst` (4 hex digits, when the frame has a Zigbee
      network header in the clear: a secured NWK payload still has its header readable), `secured` (the MAC, NWK or APS
      layer is secured), `len` (the PSDU's, its FCS included). `--freq` picks the PHY, O-QPSK at 2.4 GHz and the BPSK PHY
      below 1 GHz, and the channel the frames are noted on. The channel is at 0 Hz of the input. The receiver's minimum
      rate is 4 MHz. (The sub-GHz BPSK PHY has no test signal here and is not tested.)
    - `thread` (`thread.frame`, `zigbee::parse` then `thread::analyse`): `frame_type`, `seq`, `pan`, `src`, `dst` and `len`
      as `zigbee`'s, `secured` (the link layer is: MAC security, or an AES-CCM level OnAir says is encrypted), and `kind`,
      OnAir's own line for what the frame is, as `thread::analyse` words it (`Thread beacon, network "HomeThread", ...`,
      `Thread data, link-layer AES-CCM encrypted (...)`, `6LoWPAN IPHC, ... MLE Discovery Request (not secured)`). Thread
      has no Zigbee network layer, so there is no `nwk_src` or `nwk_dst`. 2.4 GHz channels only (OnAir's Thread tuning).
    - `zwave` (`zwave.frame`, `zwave::parse`): `home_id` (8 hex digits), `src` and `dst` (node ids as integers; 255 is the
      broadcast; a multicast has no `dst`), `frame_type` (`singlecast`, `multicast`, `ack`, `routed`), `rate_bps` (9600,
      40000 or 100000), `freq_hz`, `secured` (an S0 or S2 payload, which stays encrypted) and `len` (the MPDU's, its
      checksum or CRC included). OnAir's Z-Wave packet carries the data rate and the MPDU and **not the channel**, so
      `freq_hz` is found: the one channel of the region that the input covers (within 0.45 of the rate of the centre) and
      that uses the frame's rate, in Hz. A region with two channels of the same rate inside the input (Japan and Korea:
      R2 on three) gives no `freq_hz`. The region: `--region` when given; otherwise the region of OnAir's table with the
      most channels inside the input (its order: EU, US, ANZ, HK, IN, RU, CN, IL, JP, KR, the first winning a tie),
      which is the receiver's own pick; the receiver is told only a region the operator named. With no region having a
      channel inside, it exits 2, before reading input, with
      `onair-rx: zwave: no region has a channel within <lo>-<hi> MHz` (`--freq` plus or minus 0.45 of the rate: the
      receiver's own test); a `--region` with no channel inside with
      `onair-rx: zwave: region <region> has no channel within <lo>-<hi> MHz`, and an unknown one with
      `onair-rx: zwave: no region <name> (EU, US, ...)`. OnAir's receiver wants 1 MHz at least; a job runs at 2 MHz so
      that the EU region's three channels (868.42, 868.40 and 869.85 MHz) fit around 869.13 MHz. At 1 MHz around a
      channel (868.41 MHz) the receiver decodes the two channels inside it.
    - `wifi` (`wifi.frame`, `wifi::parseFrame`): a line for a beacon, a probe response or a FILS discovery frame only, the
      frames OnAir's parser reads an access point from; every FCS-good frame (a data frame too) is a pcap record.
      `frame_type` (`beacon`, `probe_response`, `fils_discovery`), `bssid` (12 hex digits), `ssid` (absent when the
      access point is hidden), `channel` (the beacon's own channel element, else the channel of the radiotap frequency),
      `freq_hz` (the radiotap frequency, from `--freq`), `security` (OnAir's word: `Open`, `WEP`, `WPA`, `WPA/WPA2`,
      `WPA2`, `WPA2/WPA3`, `WPA3`, `OWE (Enhanced Open)`, an enterprise one with `-Enterprise`; a FILS discovery frame
      has none) and `len` (the 802.11 frame's, without its FCS). **No `rate_mbps`:** OnAir's radiotap header has the flags,
      the channel and the signal level, and no data rate, so the rate a frame was received at is not known. The receiver
      wants 20 MHz at least; `--freq` picks the band and the channel the frames are noted on.
    - `wisun` (`wisun.frame`, `wisun::parse`): OnAir's Wi-SUN FAN monitor (IEEE 802.15.4g SUN FSK) on every channel of the
      band plan within 0.45 of the rate around `--freq`, at the bit rates the plan's channels carry (50, 100 and
      150 kbit/s; 200 and 300 kbit/s, on 600 kHz channels, are not demodulated), all at once: the nodes hop, and nothing is
      followed. `frame_type` (the Wi-SUN frame type of the UTT-IE: `pan_advert`, `pan_advert_solicit`, `pan_config`,
      `pan_config_solicit`, `data`, `ack`, `eapol`, `lfn_pan_advert`, `lfn_pan_advert_solicit`, `lfn_pan_config`,
      `lfn_pan_config_solicit`, `extended_data`; a frame without one has its MAC frame's type, as `zigbee`'s), `seq`,
      `pan` (4 hex digits: the destination's PAN, else the source's), `src` and `dst` (16 hex digits, the EUI-64; 4 for a
      short address; a broadcast has no `dst`), `freq_hz` (the channel's centre), `rate_bps` and `len` (the MPDU's, without
      its FCS: the pcap record's length). OnAir's Wi-SUN packet is the MPDU only, so the channel and the bit rate are
      taken from the receiver's own frame log (its report, `telemetry()`), where the frame is found by its octets. A
      frame whose header OnAir cannot read is a record and no line. Encrypted frames (PAN configurations, data) stay
      encrypted: no key is loaded. The band plan: `--region` when given, otherwise the receiver's own pick for `--freq`
      (OnAir's `bandPlanFor`: ANZ from 914.5 to 929 MHz and everywhere it has no other, NA from 900 to 914.5 MHz, EU863,
      EU870, CN470, CN779, WW at 2.4 GHz). A plan with no channel inside the input, as the receiver's first report says,
      exits 2 before reading input with `onair-rx: wisun: region <id> has no channel within <lo>-<hi> MHz` (named) or
      `onair-rx: wisun: region <id> (OnAir's pick for <freq> MHz) has no channel within <lo>-<hi> MHz` (`--freq` plus or
      minus 0.45 of the rate); an unknown `--region` with `onair-rx: wisun: no region <name> (ANZ, NA, ...)`. The
      receiver wants 2 MHz at least (a few channels: at 2 MHz around 921.5 MHz, the four 400 kHz channels from 921.0 to
      922.2 MHz and the 200 kHz ones from 920.8 to 922.2 MHz).
      **Its decoder runs on a thread of its own.** The receiver's feed thread finds the bursts (a filter bank, an energy
      detector per bin) and queues them; the decoder thread demodulates them; a later `feed()` or report hands the frames
      over to the hub and the frame log. A frame's line would then come at whichever block the threads got to, so
      onair-rx follows every block with the receiver's `flush()`, which waits for the decoder to finish the bursts queued
      so far and hands their frames over: a frame's line comes after the block in which its burst ended, whatever the
      threads did (one CPU or several give the same bytes), and nothing is dropped when the input comes faster than real
      time (the receiver's `setOffline(true)`: it waits for its decoder instead of dropping bursts). At the end of input
      (`flushAtEnd`) a last `flush()` hands over the rest. A burst the input cuts off is never queued, so a cut in the
      middle of a frame loses that frame and the flush does not wait for it.
      **`t_sample`** is then the first sample of the block of 16384 in which the receiver saw the burst end: 160 bin
      samples (0.32 ms at 2 MHz) of quiet after the frame's last. It is up to one block (8.2 ms at 2 MHz) before that,
      never after: on `onair-gen`'s scene at 2 MHz, from 7.0 ms to 0.2 ms before the end of the frame (its frames last
      2.0 to 7.5 ms, so from 4.5 ms before to 5.4 ms after the frame's start). From a pipe the blocks are what each read
      returns, so `t_sample` is as near or nearer.
      **`--drop`** runs the receiver as it runs live (`setOffline(false)`): no `flush()` after each block, bursts that come
      while the decoder queue is full (`kQueueMax`, 8000000 bin samples) are dropped. A frame's line then comes with the
      block in which its frame was handed over, and the report its `freq_hz` and `rate_bps` are read from may not have the
      frame yet (OnAir reports every 0.25 s of input): those two are then left out. The flush at the end of input still
      hands over the last frames; it waits for the bursts already queued, at most that queue. Without `--drop` it waits for
      one block's bursts at most. The node uses `--drop` only for a job that cannot keep real time otherwise.

  Measured on the build host (12 cores, x86-64), OnAir `a63c3a8`, user CPU per second of input (`/usr/bin/time -f %U`)
  on `onair-gen`'s scenes: `ble` at 4 MHz, 0.09 to 0.10 s a second; `lorawan` at 2 MHz around 868.3 MHz (EU868: seven
  channels, SF7 to SF12, both IQ senses, and the FSK channel), 0.17 to 0.18 s a second. Both run inside `feed()`. The
  four of the hub packets, on their scenes and (second figure) on noise alone, at the receiver's own minimum rate, which is
  also the lowest that decodes the scene: `zigbee` at 4 MHz, 0.49 and 0.59 s a second; `thread` at 4 MHz, 0.48 and 0.61;
  `zwave` at 2 MHz around 869.13 MHz, 0.01 and 0.02 (at 1 MHz around 868.41 MHz it decodes the two channels inside); `wifi` at
  20 MHz, 0.31 and 0.52. All publish from inside `feed()`. `wisun` at 2 MHz around 921.5 MHz, both threads together, 0.07
  and 0.06 s a second (0.14 to 0.15 s over 2.13 s, 0.49 to 0.51 s over 8 s of noise), the same with `--drop`: the
  `flush()` after each block costs nothing measurable with the scene's tables (OnAir's report copies its tables of nodes,
  links and the last 800 frames each time, so a busy network costs more).

  Not on the generic path: none of the modes so far. A receiver that needs more than a channel, a centre and a type
  filter (a second channel, an engine of its own) gets a line here saying what it needed.
- **stdout, polled modes:** some OnAir receivers put nothing on the bus and offer only a snapshot of their state
  (`telemetry()`). A polled mode feeds its receiver blocks of 16384 samples, each one whole however the input arrives
  (only the last may be short), and polls at the multiples of the mode's poll interval from sample 0, each after the
  block that reaches one, so a replay gives the same lines as a live run, byte for byte. stdin is read on a thread of
  its own into a queue of whole blocks (however small the reads), from which the blocks are taken, so that a session
  that waits for its receiver (`tetra`, `gsmr`) does not hold up the writer meanwhile. The queue is the mode's own
  (`PollMode::readAheadSec`): two of its receiver's stretches for a mode whose session waits (`tetra` 4.6 s, `gsmr`
  4.4 s: 70 MB at 2 MHz), two blocks for the others and under `--drop`, never more than 256 MB. When it is full the
  reader waits, and the writer with it (back-pressure; no sample is dropped), except under `--drop`: a block that finds
  the queue full is dropped, the samples after it keep their index on the input (`t_sample`), and at the end stderr says
  `onair-rx: <mode>: <n> samples dropped` (only when some were). Peak RSS from a file: `dvbt` at 10 MHz 33 MB, as
  before the queue; `gsmr` at 2 MHz 177 MB (108 MB without the queue). `ONAIR_RX_TEST_BLOCK_DELAY_US` (a test knob,
  `test/only.sh`) sleeps that long after each block, to make a session slower than its writer. An interval shorter than a
  block is a poll after every block: `dmr` at 48 kHz (0.25 s is 12000 samples) is polled every block, each 0.34 s
  of input. A poll writes one JSON object
  per line for what is new or has changed since the last poll; one more poll runs at the end of input, so that what
  the last block completed is written. Every line has `event` (the mode's) and `t_sample` (the index of the first
  sample of the block after which the poll ran), a member only when its value is known, whole numbers without a
  decimal point. Text the receiver hands over in UTF-8 (`ps`, `rt`, `alias`, a DVB-T service's `name`) is written as
  UTF-8, escaping only a quote, a backslash and a control character; text that is not valid UTF-8 is written byte by
  byte as above, so a line is always valid JSON. Only `tetra` and `gsmr` have packets (a row's hub channel): with
  another polled mode `--pcap-port` exits 2 with `onair-rx: <mode> has no packets (no --pcap-port)`, before reading any
  input. The modes:

  | mode | OnAir receiver | minimum rate | poll | event | members |
  |---|---|---|---|---|---|
  | `rds` | `FmReceiver` | 500 kHz | every block | `rds.group` | `pi`, `group`, `pty`, `tp`, `ps`, `rt` |
  | `dmr` | `DmrReceiver` (`configureChannel`) | 48 kHz | 0.25 s (every block, 0.34 s, at 48 kHz) | `voice.call` | a call: `type`, `src`, `dst`, `call`, `color_code`, `slot`, `encrypted`, `emergency`, `silent`, `alias`, `eot`; a channel: `type`, `color_code`, `control`, `network` |
  | `p25` | `P25Receiver` (phase 1) | 250 kHz | 0.25 s | `voice.call` | a call: `type`, `src`, `dst`, `call`, `nac`, `encrypted`, `emergency`, `silent`, `algid`, `kid`, `eot`; a channel: `type`, `nac`, `control`, `network` |
  | `m17` | `M17Receiver` | 250 kHz | 0.25 s | `voice.call` | `src`, `dst`, `can`, `type`, `encryption`, `lat`, `lon`, `eot` |
  | `dvbt` | `DvbtReceiver`, `TsDemux` | 7.9 MHz for 8 MHz (4.9375 MHz for 5) | 0.25 s | `dvbt.status` | `locked`, `lock`, `bandwidth_hz`, `mode`, `guard`, `constellation`, `code_rate`, `hierarchy`, `lp_code_rate`, `snr_db`, `services` |
  | `video` | `AtvReceiver` (FM video) | 8 MHz | 0.25 s | `video.status` | `system`, `standard`, `locked`, `lock`, `line_hz`, `snr_db` |
  | `tetra` | `TetraReceiver` | 200 kHz (2 MHz usual) | 0.25 s | `tetra.cell` | `freq_hz`, `mcc`, `mnc`, `colour_code`, `la`, `network_time`, `neighbours`, `snr_db` |
  | `gsmr` | `GsmrReceiver` | 1 MHz (2 MHz in the node's row, 5 MHz OnAir's usual) | 0.25 s | `gsmr.cell` | `freq_hz`, `arfcn`, `bsic`, `mcc`, `mnc`, `lac`, `cell_id`, `snr_db` |

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
  - `dvbt`, `video` (the status modes): a line says the receiver's state, what it has locked to and what the signal
    says about itself. The first line comes at first lock or at 2 s of input (the poll of the block that reaches 2 s),
    whichever is first, so a signal that locks by then has a locked first line; input that ends sooner without a lock
    has its one line from the poll at the end (`locked` false), and empty input none; then a line comes when any member of
    the state changes, and one at least every 2 s of input, so a watcher always has a
    current one. A measured value (`snr_db`, `line_hz`) is written on each line but makes no line of its own: it moves
    at every poll. A poll is every 0.25 s, so the 2 s line is written at the last poll before the gap could pass 2 s:
    lines are 1.75 s apart on a signal that does not change, never more than 2 s. On a signal that never locks (noise,
    the wrong width, a DVB-T2 multiplex) the lines say so (`locked` false) every 1.75 s, and the receiver keeps
    searching. `locked` and `lock` are on every line; the values are spelled as ZetaSDR's rows spell them:
    - `dvbt`: OnAir's DVB-T receiver on one channel width at a time, the multiplex at 0 Hz of the input. Without
      `--bandwidth` it tries 8, 7, 6 and 5 MHz in turn (those the rate takes: 7.9 MHz of input for 8 MHz, scaled with
      the width, OnAir's floor, and that OnAir's resampler can bring the rate to: a width whose own rate it finds no
      ratio to, within 1e-4 and 1024 phases, is not tried, as the receiver would search on it for ever; 9138287 Hz is
      such a rate for 8 MHz. With no width left the rate exits 2, before reading input, with `onair-rx: <rate> Hz
      cannot be resampled for dvbt at <widths> MHz (OnAir's resampler finds no ratio)`), each for 2 s of input (eight polls, so the widths change on the 2 s marks), until the
      TPS is decoded. The width is decided at each poll from the same snapshot as the line, after it: a poll whose
      line says `locked` holds the width, and eight polls in a row that say `searching` (counted again from a lost
      lock) move to the next. So a line never carries one width's signalling with another's `bandwidth_hz`, and a lock
      that comes just before a width's 2 s are up keeps it. `--bandwidth` fixes the width. `lock` is
      `locked` (the TPS decoded) or `searching`, as the row's own program, dvbt-rx, has them; everything else is
      written only while locked, spelled as dvbt-rx (SDRangel's names) spells it: `bandwidth_hz` (the width the lock
      is on, in Hz), `mode` (`2K`, `8K`, or DVB-H's `4K`), `guard` (`1/32`, `1/16`, `1/8`, `1/4`), `constellation`
      (`QPSK`, `16-QAM`, `64-QAM`), `code_rate` (the high-priority stream's: `1/2`, `2/3`, `3/4`, `5/6`, `7/8`),
      `hierarchy` (`a=1`, `a=2`, `a=4`) and `lp_code_rate` (only for a hierarchical multiplex), `snr_db` (OnAir's
      channel estimate's signal to noise, one decimal, once the receiver has measured it for this lock: the line of a
      lock just made may have none) and `services`. `cell_id` is never written: OnAir's TPS
      decoder does not read the cell id bits. `services` is the transport stream's services as OnAir's
      demultiplexer (`TsDemux`) has read them from its tables: an array of `{"id": <service id>, "name": "<name>",
      "kind": "tv"|"radio"|"data"}`, the name once the SDT gave it (UTF-8: OnAir reads it from the broadcast's
      character table), the kind once the service type is known (a teletext or other service is `data`). A service is
      listed once any of the PAT, its PMT or the SDT has named it, in OnAir's order (by logical channel number, then
      id); the member is absent while none has. The decoded packets go to the demultiplexer only. The receiver runs
      inside `feed()` (OnAir's default, not its error correction thread), so the packets of a block are read before
      its poll. It opens no sound device. The Viterbi decoder runs on the CPU (`setComputeMode(0)`; OnAir has no GPU
      decoder on Linux anyway).
    - `video`: OnAir's analog TV receiver in its FM video mode (analog FPV links and the like: the carrier is the
      centre of the input, the sync polarity found by trial), silent. `system` and `standard`, once the line count is
      known, are `PAL 625/50` and `pal`, `NTSC 525/60` and `ntsc`, or `SECAM 625/50` and `secam` (625 lines with
      SECAM colour; the receiver tells colour only when its picture rate is 9.5 MHz or more, which with its decimation
      by the nearest whole number to the input rate over 10 MHz means 10, 12 or 20 MHz of input, not 8 or 16, so at
      8 or 16 MHz a SECAM picture is `pal`); `locked` is line and field sync, `lock` `locked` or `no lock`; `line_hz`
      (the measured line rate, whole Hz) and `snr_db` (sync against the noise on the porches, whole dB) are written
      once there is line sync. No picture is taken (the receiver still decodes one, which is most of its cost).
  - `tetra`, `gsmr` (the cell modes, `src/poll_cell.cpp`): the base stations on the input, from what each broadcasts in
    the clear to every terminal on its downlink. `--freq` is required (each cell's carrier is the radio frequency plus
    its offset; without it the modes exit 2 with `onair-rx: <mode> needs --freq HZ`), and the whole input is searched
    for carriers, as the receivers do by default (TETRA: 25 kHz carriers in the middle 72 % of the input; GSM-R: 200 kHz
    carriers, the strongest 12, outside 4 % of each edge). Receive only: nothing addressed to a terminal is decoded,
    nothing is decrypted, no key is loaded.
    - `tetra` (`tetra.cell`): `freq_hz` (the main carrier's frequency the cell announces in SYSINFO, when this carrier is
      it; else the carrier as measured, to the hertz), `mcc`, `mnc` and `colour_code` (the synchronisation burst: the
      receiver keeps no cell without them), `la` (the location area, from SYSINFO), `network_time` (OnAir's text of the
      neighbour broadcast's network time, `2026-10-12 09:30:00 UTC, local +03:00`, when the broadcast carries one),
      `neighbours` (how many neighbour cells the cell has broadcast, once it has) and `snr_db` (one decimal). Not
      `setSingleCarrier`: every carrier in the input is a cell of its own.
    - `gsmr` (`gsmr.cell`): `freq_hz` (the ARFCN's own downlink frequency; off the raster, as measured), `arfcn` (when
      the carrier is on the raster of GSM 850, 900, DCS 1800 or PCS 1900: OnAir's `gsmrHzToArfcn`), `bsic` (the SCH's base
      station identity code, NCC times 8 plus BCC: 1-2 is 10), `mcc`, `mnc` and `lac` (System Information 3 or 4),
      `cell_id` (System Information 3) and `snr_db` (one decimal). Paging and access blocks are only counted by the
      receiver; none of it is written.
    A cell gives a line when it is first read and one when a member of it changes. A cell is keyed by its carrier and its
    identity (TETRA: MCC, MNC and colour code, the carrier within 5 kHz of where it was; GSM-R: ARFCN and BSIC, within 60
    kHz off the raster), the receivers' own rules, so a carrier measured a few hertz off in each stretch stays one cell.
    What is measured (`snr_db`, a carrier's measured `freq_hz`) and TETRA's clock (`network_time`'s value; whether there
    is one counts) is written on each line and makes no line of its own, as `dvbt`'s `snr_db`: a cell that stays on the
    air gives no line per poll. A cell no longer heard gives no line. The line comes at the first poll after the stretch
    that read it (below).
    **The receivers decode a stretch of input at a time, on a worker thread of their own:** TETRA 2.3 s
    (`TetraReceiver::kCaptureSec`), GSM-R 2.2 s (`kBlockSec` in `gsmr_rx.cpp`). Live they skip what comes while the worker
    is busy (TETRA gathers the next stretch meanwhile and skips only once that is full; GSM-R gathers only while the
    worker is idle, and also drops the rest of the block that completes a stretch). A replay faster than real time (ZetaSDR's
    `capture_decode`) would lose most of its input that way, and which part would depend on the worker's timing. So the
    session waits for the worker (`busy()`; neither receiver has a switch to block) after each block (TETRA) or at each
    stretch's end, where it cuts the block (GSM-R): the receiver skips nothing, the stretches follow each other from sample
    0, each is decoded before the next sample is fed, and every line depends on the input alone (two runs, one CPU or
    several, a file or a pipe give the same bytes). A stretch's lines come at the first poll after the block that ends
    it: at most 0.25 s of input and one block later. stdin is read meanwhile (the read-ahead thread above), so a writer
    at real time is not held up by the wait. The receivers have no flush, so at the end of input the session completes
    the last partial stretch with zeros and the receiver decodes what it held: input shorter than one stretch (1.5 s of
    the scene) gives its cells' lines too, and input that ends with a whole stretch pads nothing. The last poll waits at
    most 5 s in all, for the padded stretch (and, with `--drop`, first for a stretch still being decoded), then reports
    what the snapshot has; the session's end then makes a stretch still being decoded give up at its next carrier, so the
    program never waits on it. With `--drop` what the receiver holds is not known (it skipped), so a whole stretch of zeros
    goes in, which completes it.
    **`--drop`** runs the receiver as it runs live: the session does not wait, and the receiver skips what comes while it
    decodes. Which stretches are decoded then depends on the worker's timing (from a file nearly all the input after the
    first stretch goes), and so do the lines.
    **Packets** (`--pcap-port`): each signalling block the receiver decoded, as GSMTAP in IPv4 + UDP to port 4729 (link
    type 101), as OnAir's capture files have them: TETRA's synchronisation bursts and signalling blocks (GSMTAP type 5,
    TETRA I1; OnAir's encrypted signalling as received), GSM-R's BCCH blocks (type 1, Um; the CCCH is not published).
    OnAir's GSMTAP is version 1 with a 4-word header. They come from the worker, through the hub tap's lock; `runPolled`
    drains them after every block and writes each at the drained block's first sample.

  Measured on the build host (12 cores, x86-64), OnAir `a63c3a8`, user CPU per second of input (`/usr/bin/time -f %U`
  over the input's seconds; `test/poll.sh` prints its own):
  - `dvbt` at 10 MHz, `onair-gen`'s 8 MHz multiplex (8K, 64-QAM, 3/4), locked: 0.75 to 0.79 s a second. OnAir spreads
    part of it over its worker pool, so the wall time is 0.61 s a second on all cores, 0.69 on two and 0.79 on one:
    it keeps real time on one core. With the receiver's error correction on its own thread (`setPipelined(true,
    true)`): the same user CPU (0.75 to 0.81), 0.40 s of wall time a second on all cores, 0.43 to 0.46 on two, 0.79 to
    0.81 on one. The synchronous receiver is kept, since it keeps real time either way and its lines do not depend on
    thread timing. On noise, searching every width in turn: 0.58 s a second.
  - `video`, `onair-gen`'s PAL test card with the swing of ZetaSDR's check (white 1.05 MHz above blanking: 1.5 MHz a
    volt, no sound subcarrier), and an FPV link's full swing (white 8 MHz above blanking, the generator's own; its
    sound subcarrier off), 3 s each, then 3 s of noise:

    | input rate | check swing | CPU | full FPV swing | CPU | noise, CPU |
    |---|---|---|---|---|---|
    | 8 MHz | locks (SNR 34 dB) | 0.25 to 0.26 | no lock | 0.51 to 0.53 | 0.77 |
    | 10 MHz | locks (30 to 32 dB) | 0.46 to 0.47 | locks late, then loses it | 0.61 | 0.92 to 0.94 |
    | 12 MHz | locks (30 to 31 dB) | 0.51 | no lock | 0.76 | 1.07 to 1.08 |
    | 16 MHz | locks (32 dB) | 0.30 | locks weakly (8 to 10 dB) | 0.26 to 0.28 | 0.90 to 0.91 |
    | 20 MHz | locks (29 to 30 dB) | 0.58 to 0.59 | locks (43 to 46 dB) | 0.51 to 0.52 | 1.10 to 1.11 |

    With the sound subcarrier at 6.5 MHz on, the full swing does not lock at 16 MHz either; OnAir says such a link is
    about 17 MHz wide and asks for 20 MHz. The cost is the receiver's FM search on noise more than its decoding of a
    locked picture: at 12 and 20 MHz a channel with nothing on it takes more than one core. `setColour(false)` takes
    about a third off a locked picture at 12 MHz and nothing off noise; it is not set, since it would lose SECAM.
  - `tetra` and `gsmr` at 2 MHz on `onair-gen`'s scenes (TETRA: one cell; GSM-R: two cells), user CPU and wall time per
    second of input, OnAir's worker and the feeding thread together; eleven file runs and two paced runs each, the load
    average 3 to 10 from other work. The worker takes 0.18 s a stretch for TETRA's one carrier and about 1.0 s for
    GSM-R's two (about 0.5 s a carrier: OnAir's search keeps up to 12, so a full band costs up to six times the scene's). On 8 s of noise (no carrier) both cost
    0.01 s a second.

    | mode, input | user CPU a second | wall a second | keeps real time |
    |---|---|---|---|
    | `tetra`, file | 0.07 | 0.09 to 0.10 | yes (every stretch decoded) |
    | `tetra --drop`, file | 0.03 | 0.03 | yes, but it decoded 1 of the 3 stretches here |
    | `tetra`, paced at real time | 0.05 to 0.08 | ends 0.1 to 0.2 s after the input | yes; a writer of 16384-sample blocks is held up 4 ms at most |
    | `tetra --drop`, paced at real time | 0.08 | ends with the input | yes (every stretch decoded) |
    | `gsmr`, file | 0.43 to 0.45 | 0.48 to 0.50 | yes (every stretch decoded) |
    | `gsmr --drop`, file | 0.15 | 0.17 | yes, but it decoded 1 of the 3 stretches here |
    | `gsmr`, paced at real time | 0.44 to 0.49 | ends 0.9 to 1.0 s after the input | yes; a writer of 16384-sample blocks is held up 10 ms at most |
    | `gsmr --drop`, paced at real time | 0.27 to 0.31 | ends with the input | yes, but it skips about a second after each stretch (2 of 3 decoded) |

    Without `--drop` the session waits while a stretch is decoded; the read-ahead thread keeps reading stdin meanwhile
    (before it, a writer at real time was held up 0.17 to 0.31 s by TETRA's wait and 1.0 to 1.4 s by GSM-R's), and the
    session catches up on the queued input after. The queue's two stretches are well above a stretch's decoding here; a band whose
    stretch takes longer to decode than it lasts cannot keep real time without `--drop`. The paced figures (input end
    and CPU) are from before the read-ahead thread.
- **`--pcap-port PORT`:** listens on 127.0.0.1:PORT before reading stdin and serves one client at a time a classic
  pcap stream: the file header, then one record per frame. ADS-B: 7 or 14 bytes, link type 147 (USER0), and every
  JSON line has exactly one record. APRS: the AX.25 frame as OnAir's Wireshark hub gets it (link type 3), also a
  digipeater's copy, which has no line. AIS: each sentence with its line end in IPv4 + UDP to port 10110 (NMEA 0183),
  link type 101 (raw IP), as OnAir's capture files have it. Bluetooth LE: every good packet, the advertising access
  address and the dewhitened PDU and CRC (link type 251, BLUETOOTH_LE_LL), also an extended advertisement, which has no
  line. LoRaWAN: the PHY payload of every frame that has a line, behind a LoRaTap header (frequency, bandwidth, spreading
  factor, SNR; link type 270). Zigbee and Thread: the PSDU with its FCS (link type 195, IEEE 802.15.4 with FCS). Z-Wave: a
  byte for the data rate (0 to 2) and the MPDU (link type 148, USER1; `onair-rx --lua` has the dissector). Wi-Fi: a radiotap
  header and the 802.11 frame, FCS taken off (link type 127), every good frame, also those without a line. Wi-SUN: the
  MPDU as received, without its FCS (link type 230, IEEE 802.15.4 without FCS), one per frame. TETRA and GSM-R: each
  signalling block in GSMTAP, inside IPv4 + UDP to port 4729 (link type 101). Each of these
  has at least as many records as lines. The other bus modes and the other polled modes have no packets and refuse the option (exit 2). Records wait in a bounded queue (8192, the oldest dropped first), so a client that
  connects a few seconds late still gets them; a client that hangs up is dropped, and the next one gets a new header.
  Record times are the frame's sample time (bus modes: its block's first sample) on the wall clock of the first sample,
  moved on when the input falls more than 100 ms behind real time. What is still queued at the end with no client is
  counted as dropped. The decoding never waits for the port.
- **stderr:** diagnostics, one line each. An unknown mode exits 2 with `onair-rx: no mode <m>`; a port that
  cannot be opened, or a failed read of stdin (`onair-rx: stdin: <reason>`), exits 1.
- **EOF** on stdin: what was decoded is written (the queued records to a connected client), then exit 0.
  Only `wisun` flushes its receiver at the end of input (a bus row's `flushAtEnd`, below, is for one that holds frames
  until told); with another mode a decode a receiver still holds when stdin closes is not written, and a short
  replay (ZetaSDR's `capture_decode`) can lose its last message this way. A polled mode polls once more at EOF, so
  what its receiver's snapshot has by then is written; `tetra` and `gsmr` first complete their last partial stretch with
  zeros and decode it, within 5 s.
  SIGTERM ends it at once.

## Build

```
ONAIR_SRC=<OnAir checkout> ./build.sh     # the first time: OnAir headless in _onair-build (about 20 minutes)
./build.sh                                # then: only src/ is compiled and linked (seconds)
test/patch.sh                             # the patches apply, _onair-src is current, and the patch's own test passes
test/run.sh                               # needs iq/adsb1090.sigmf-data (cf32, 2457600 Hz); skips the decode checks without it
test/bus.sh                               # the bus modes on OnAir's own test signals (_build/onair-gen)
test/poll.sh                              # the polled modes on OnAir's own test signals (_build/onair-gen), the voice
                                          # modes also with mbelib and Codec 2 hidden, the status modes also on noise
                                          # (about 40 s; it prints each status run's user CPU)
test/only.sh                              # the modes only OnAir decodes (ble, lorawan, zigbee, thread, zwave, wifi,
                                          # wisun, tetra, gsmr) on OnAir's own scenes, their packets, the options each
                                          # mode refuses, noise alone (it prints each mode's user CPU), onair-gen's
                                          # --sigmf pair; wisun also cut short, run twice and on one CPU and two
                                          # (taskset); tetra and gsmr run twice, on one CPU, through a pipe, with --drop,
                                          # on 1.5 s of input and at real time through a pipe
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
take is refused with the usage line.
`onair-gen --mode dvbt --rate HZ [--bandwidth 8|7|6|5] [--service NAME] [--lead SEC]` is 6 s of one multiplex of
OnAir's DVB-T generator, 8 MHz wide (or the width given), 8K, guard 1/8, 64-QAM, code rate 3/4, with one television
service, id 4660, named `ZETA TEST` (or the name's bytes as the SDT carries them: a first byte below 0x20 names the
character table), made at the width's native rate, resampled to the rate and with noise 30 dB down; `--lead` puts that
many seconds of the noise alone before it (`test/poll.sh` uses it to have the lock land at the end of the 8 MHz
width's 2 s). `onair-gen --mode video --rate HZ
[--deviation HZ]` is 3 s of a PAL test card on FM from OnAir's FPV generator with the swing of ZetaSDR's check (white
1.05 MHz above blanking) and no sound subcarrier; `--deviation` is white's offset instead (OnAir's FPV link: 8000000).
Both are broadcasts: the whole output is the signal, with nothing around it.
`onair-gen --mode ble|lorawan --rate HZ [--freq HZ]` is the generator's own scene, with a second of noise either side
(25 dB carrier to noise in 1 MHz for Bluetooth LE, in 125 kHz for LoRaWAN): Bluetooth LE, 0.25 s of OnAir's
advertisers (`makeBleSynth`: a phone `C0:FF:EE:12:34:56` named `OnAir`, an iBeacon `00:0B:A1:5E:00:01`, an Eddystone
`3A:55:44:33:22:02`, a resolvable address `5B:12:34:56:78:9A`, a scanner `66:55:44:33:22:11` sending a scan request and
a connection request, and an extended advertisement), whitened for channels 37, 38 and 39 in turn, at 0 Hz; LoRaWAN,
OnAir's EU868 scene once (`makeLorawanSynth`: data uplinks from DevAddr 26011A2B, 26014C01, 2701AAAA (250 kHz) and
27023C00, a join request and its join accept, a confirmed uplink from 260B1234 and its answer, a proprietary frame, and
the FSK channel's frames), on its channels around `--freq` (default 868.3 MHz, the scene's centre; the frames stay on
their channels). `--freq` of `ble` (default 2402 MHz) only goes into a `--sigmf` pair.
`onair-gen --mode zigbee|thread|zwave|wifi --rate HZ [--freq HZ]` likewise, each once: Zigbee, OnAir's schedule
(`makeZigbeeSynth`: PAN `1A62`, the coordinator `0000`, a beacon request, its beacon, an association of the bulb
`00:17:88:01:0A:0B:0C:0D`, an ack, a Device_annce from `1234` in the clear, a link status, encrypted NWK frames, a second
PAN `3C2D`) with noise 25 dB down in 2 MHz; Thread, the frames of OnAir's Thread test (a beacon of the network
`HomeThread`, PAN `7A31`, from `5000`, six link-layer encrypted data frames from `5400` and `5401` to `5000`, and an MLE
Discovery Request in the clear from `01:23:45:67:89:AB:CD:EF`), 2 MHz likewise; Z-Wave, OnAir's EU network
(`makeZwaveSynth`: home ID `C0FFEE01`, controller 1, nodes 2 to 6, frames at R1, R2 and R3 on 868.42, 868.40 and
869.85 MHz, acks, a multicast, an S0 and an S2 frame) around `--freq` (default 869.13 MHz, the region's centre), noise in
100 kHz; Wi-Fi, OnAir's 2.4 GHz scene (`makeWifiSynth`: "OnAir-Home", WPA2, `00:04:0E:12:34:56`, with a probe response;
"OnAir-Cafe", WPA2/WPA3, `FC:EC:DA:AB:CD:EF`; a hidden open access point `02:11:22:33:44:55`; a data frame; 1 Mbit/s on
channel 6), noise in 20 MHz and **a tenth of a second** of it either side (at 20 MHz a second is 160 MB). The
`--freq` defaults of Zigbee and Thread (2425 MHz, channel 15) and Wi-Fi (2437 MHz, channel 6) only go into a `--sigmf` pair;
Z-Wave's moves the channels in the band. Only the 2.4 GHz Wi-Fi scene is used: the 5 and 6 GHz paths of the receiver
(OFDM, several channels in a wide input) are not exercised here.
`onair-gen --mode wisun --rate HZ [--freq HZ]` likewise: OnAir's FAN scene once (`makeWisunSynth`, `wisunTestSchedule`:
PAN `7A31`, the border router `00:12:4B:FF:FE:1A:2B:3C` and the router `00:0D:6F:FF:FE:44:55:66` advertising, the meter
`02:A1:B2:FF:FE:C3:D4:E5` soliciting and its EAPOL exchange through the router, a PAN configuration solicit and two PAN
configurations, three data frames to the router and its acks, the border router's broadcast data, all at 150 kbit/s on
the ANZ 400 kHz channels within 0.4 of the rate of 921.5 MHz, and a PAN configuration solicit at 50 kbit/s on a 200 kHz
channel), on its channels around `--freq` (default 921.5 MHz, the scene's centre, between two channels; the frames stay
on their channels), noise in 400 kHz (the generator's own measure). At 2 MHz that is 16 frames on 921.0, 921.2 and
922.2 MHz. After its message line stderr says where each frame is: `frame N from A s to B s, R bit/s on F Hz`.
`onair-gen --mode tetra|gsmr --rate HZ [--freq HZ]` is a downlink, written as it is made with the generator's own noise (25
dB in 25 kHz for TETRA, in 200 kHz for GSM-R), for three of the receiver's stretches and 0.3 s more (7.2 s, 6.9 s):
TETRA, OnAir's scene 0 (`makeTetraSynth`: one cell on 392.4125 MHz, MCC 1, MNC 1, colour code 5, LA 1001, two neighbours,
the network time, encrypted signalling; at 4 MHz and more OnAir's wide scene of 12 cells instead) around `--freq`
(default 392.5 MHz, the scene's centre); GSM-R, OnAir's scene 0 (`makeGsmrSynth`: ARFCN 960 on 922.2 MHz and 968 on
923.8 MHz, BSIC 1-2 and 1-5, MCC 1, MNC 1, LAC 4711, cells 12001 and 12002) around `--freq` (default **923.0 MHz**, the
middle of the two: at the scene's own centre, 922.8 MHz, ARFCN 968 is at the edge of a 2 MHz input and is not found).
The cells stay on their frequencies. After its message line stderr says what each cell inside the part of the input its
receiver searches broadcasts, as onair-rx's line names it: `cell N at F Hz: mcc 1, mnc 1, colour_code 5, ...`.
`onair-gen ... --sigmf STEM`, with any mode, writes `STEM.sigmf-data` (ci16_le: each value times 32768, rounded and
clipped) and `STEM.sigmf-meta` instead of stdout, the meta in the shape of `iq/adsb1090.sigmf-meta`: the sample rate,
the datatype, the centre frequency (`--freq`, or the mode's own: ACARS 131.525 MHz, AIS 162 MHz, Bluetooth LE 2402 MHz,
LoRaWAN 868.3 MHz, Zigbee and Thread 2425 MHz, Z-Wave 869.13 MHz, Wi-Fi 2437 MHz, Wi-SUN 921.5 MHz, TETRA 392.5 MHz,
GSM-R 923.0 MHz; left out for a mode with neither) and an annotation (`message`) per message: where the stderr line
says it is.
`onair-gen --selftest ais-join` tests the AIS sentence joiner of `src/ais.cpp`, which it links,
`onair-gen --selftest sonde-id` the spelling of a sonde's `id` (`sondeId`, `src/bus.h`)
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
  all: a tap on the bus and a tap on the hub (`HubTap`, below) collect what a block's `feed()` produced, then the
  events become lines and the packets records. A mode is a row of `kBusModes`: how to make and set up its receiver,
  its hub channel, whether it needs `--freq`, and what its lines need that the bus lacks. A mode whose lines come
  from its hub packets (AIS) names the lines' event and a function that turns a packet into a line; its hub channel
  then runs with or without a pcap port. The row's `packetLines` makes that function once per run, after `open`, from the
  run's options and the feed `open` made: Z-Wave's lines need to know where the input is (its packets carry no channel),
  Wi-SUN's need the receiver's frame log, which they reach through its feed (a `WisunFeed`, `std::function::target`).
  A row with `flushAtEnd` has its feed called once more at the end of input with no samples (which its feed takes as
  "flush"), and what that gives is drained at the last block's first sample. Another receiver on the bus is one more row.
- `src/frames.cpp`: the `packetLines` of `zigbee`, `thread`, `zwave`, `wifi` and `wisun` (declared in `src/frames.h`), each
  turning a hub packet into the members of a line with OnAir's own parser (`zigbee::parse`, `thread::analyse`,
  `zwave::parse`, `wifi::parseFrame`, `wisun::parse`), and Z-Wave's region choice.
- `src/hub_tap.cpp`: `HubTap`, the hub's packets of one channel: it configures the hub (no file, no live capture, no
  GSMTAP), puts its sink on it (`patches/0001`) and enables the channel; the sink collects under a lock, so a receiver
  may publish from a thread of its own, and the read loop drains it after each block. `hubRecord` makes a packet a
  pcap record body: as it is on a Direct channel, in IPv4 + UDP to the channel's port otherwise.
- `src/poll.cpp`: the polled modes. A mode is a row of `kPollModes`: its name, its event, its poll interval and the
  function that makes its session (`src/poll_<mode>.cpp`), a `PollSession` that feeds the receiver and, at each poll,
  hands over the lines for what is new or changed in its snapshot; `runPolled` adds `event` and `t_sample`. RDS
  (`src/poll_rds.cpp`) queues a line per group from the receiver's group tap and is polled after every block. The
  voice modes (`src/poll_voice.cpp`) are polled every 0.25 s (or after every block, when that is longer): each turns its receiver's call log into what it says
  about each call (a key and the members) and hands that to a `CallLog` (`src/calls.h`), which writes the first, the
  changed and the last line of each call. The status modes (`src/poll_tv.cpp`) are polled every 0.25 s and write
  their receiver's state on a change and at least every 2 s; DVB-T's session also steps through the channel widths
  as it feeds, and reads the service list from a `TsDemux` its packet callback feeds. The cell modes
  (`src/poll_cell.cpp`) are polled every 0.25 s: each waits for its receiver's worker as it feeds (unless `--drop`),
  turns the receiver's cell table into what it says about each cell (its identity, its carrier, the members compared
  and the line) and hands that to a `CellLog`, which writes a cell's first line and its changed ones. A row with a hub
  channel (`chan`) has a `HubTap` with a pcap port, drained after every block and after the last poll; a row with
  `needsFreq` refuses to run without `--freq`.
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
