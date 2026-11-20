# onair-rx (spike)

A GPL-3.0-or-later wrapper that runs one OnAir receiver on IQ from stdin and writes the frames it hands to
OnAir's Wireshark hub as classic pcap on stdout, so ZetaSDR can run OnAir as an external handler (a program
row) and capture its packets. Based on OnAir by Kristijonas Adomaitis - https://on-air.dev

Build: `ONAIR_SRC=<OnAir checkout> ./build.sh`. Run: `onair-rx --mode adsb --rate 2457600 --format cf32 < iq > out.pcap`;
`onair-rx --lua > onair.lua` writes OnAir's Lua dissector (USER0 Mode S, USER1 Z-Wave, NMEA port).

## Spike result, 2026-11-20 (OnAir a63c3a8, Ubuntu 24.04, g++ 13)

- Full headless build: 2418 steps, no errors (FFmpeg dev packages needed; libavdevice optional).
- 8 s of real 1090 MHz IQ from a B210 (cf32, 2.4576 Msps): 207 Mode S frames (143 long, 64 short), 0 dropped,
  6 aircraft; 0.67 s of one core (about 12x real time), 8 MB RSS. tshark with `onair.lua` dissects DF, CA,
  ICAO address, type code and parity.
- The receivers share one contract (`configure(rateHz)`, `ready()`, `feed(cf32*, n)`), so a mode table can
  cover every channel the hub knows (16 today, from 15 receivers: Wi-Fi, HaLow, Zigbee, Thread, Wi-SUN, BLE, Z-Wave,
  LoRaWAN, DVB IP, ADS-B, AIS, APRS, ASTERIX, GSM-R, TETRA, LTE).
- The hub has no stdout output; the wrapper reads the hub's own live stream (`ONAIR-WIRE 1 <mode>`, 127.0.0.1,
  any free port) and adds the pcap file header. OnAir is used unchanged.
- Timestamps are the hub's decode time, not sample time: a file read faster than real time collapses to
  wall-clock. With the node pacing stdin in real time the error is about one block (~27 ms at 64 Ki samples);
  sample time needs an upstream change (receivers pass `unixUsec` to `Hub::publish`).
- OnAir's whole engine cannot read stdin as is (its file source sizes, seeks and loops the file); covering
  modes that need the engine's own chain (rather than a bare receiver) needs a stdin `IqSource`.
- OnAir also has events (telemetry, log callbacks) per receiver; a full handler would emit those as JSON lines
  for the node's events beside the packet stream.
