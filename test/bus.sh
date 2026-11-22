#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the bus modes (pocsag, flex, acars, aprs, ais, vdl2, hfdl, rs41, dfm, m20) on OnAir's own test signals:
# _build/onair-gen makes one known message per mode (AIS: two; VDL2: three frames; HFDL: two; a sonde: 12 s of its frames) with
# a second of noise either side, onair-rx decodes it. The signals are synthesised; no capture is used. Also: a mode that needs
# --freq refuses to start without it, and AIS with a centre its channels do not fall in; every mode writes only its own bus
# event (an APRS message is one aprs.packet line), each sonde mode only its own type; an APRS object's or third-party packet's
# position is left out; text the bus cuts or that is not ASCII stays one valid line; a pcap port with no client, or with a
# client that hangs up, changes neither the lines nor the run time; AIS's sentences on the pcap port; and the AIS sentence
# joiner on its own.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
gen=${ONAIR_GEN:-$here/_build/onair-gen}
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
acars_hz=131525000
ais_hz=162000000   # the middle of AIS 1 (161.975 MHz) and AIS 2 (162.025 MHz)
vdl2_hz=136975000
hfdl_hz=8927000    # a dial frequency: the carrier is 1440 Hz above it
sonde_hz=403000000

# --- onair-rx has each mode: on empty input it ends with exit 0 and no line
for m in pocsag flex acars aprs ais vdl2 hfdl rs41 dfm m20; do
  f=() rate=1000000
  [ "$m" != acars ] || f=(--freq "$acars_hz")
  [ "$m" != ais ] || f=(--freq "$ais_hz")
  [ "$m" != vdl2 ] || f=(--freq "$vdl2_hz")
  [ "$m" != hfdl ] || f=(--freq "$hfdl_hz")
  case $m in rs41 | dfm | m20) rate=2000000 ;; esac
  "$bin" --mode "$m" --rate "$rate" --format cf32 "${f[@]}" </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" ||
    fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has pocsag, flex, acars, aprs, ais, vdl2, hfdl, rs41, dfm and m20"

# --- refuses <stderr> <onair-rx args...>: exit 2 with exactly that one line on stderr, before reading any input (stdin stays
# open and is never written), and nothing on stdout
refuses() {
  python3 -I - "$bin" "$@" <<'EOF'
import subprocess, sys
want, args = sys.argv[2], [sys.argv[1]] + sys.argv[3:]
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
    sys.exit(f"{' '.join(args[1:])} waited for input")
out, err = p.stdout.read(), p.stderr.read()
assert p.returncode == 2, f"exit {p.returncode}, not 2"
assert err == (want + "\n").encode(), f"stderr {err!r}"
assert out == b"", "it wrote to stdout"
print(f"ok: {' '.join(args[1:])} exits 2 before reading input: {want}")
EOF
}
refuses "onair-rx: acars needs --freq HZ" --mode acars --rate 1000000 || fail "acars without --freq"
refuses "onair-rx: ais needs --freq HZ" --mode ais --rate 250000 || fail "ais without --freq"
refuses "onair-rx: vdl2 needs --freq HZ" --mode vdl2 --rate 250000 || fail "vdl2 without --freq"
refuses "onair-rx: hfdl needs --freq HZ" --mode hfdl --rate 24000 || fail "hfdl without --freq"
refuses "onair-rx: 1000000 Hz is too low for rs41 (at least 2000000 Hz)" --mode rs41 --rate 1000000 || fail "rs41 at 1 MHz"
# 250 kHz at 161.8 MHz ends at 161.925 MHz, short of AIS 1; at 162.2 MHz it starts at 162.075 MHz, past AIS 2
refuses "onair-rx: ais: no AIS channel within 250000 Hz of 161800000 Hz" --mode ais --rate 250000 --freq 161800000 ||
  fail "ais below its channels"
refuses "onair-rx: ais: no AIS channel within 250000 Hz of 162200000 Hz" --mode ais --rate 250000 --freq 162200000 ||
  fail "ais above its channels"

[ -x "$gen" ] || fail "no test signal generator at $gen (run ./build.sh)"

# gen <tag> <onair-gen args...>: the IQ in $d/<tag>.iq, what the generator said (where the message is) in $d/<tag>.gen
gen() {
  local tag=$1
  shift
  "$gen" "$@" >"$d/$tag.iq" 2>"$d/$tag.gen" || fail "onair-gen $*: $(cat "$d/$tag.gen")"
}
# rx <tag> <onair-rx args...>: $d/<tag>.iq through onair-rx into $d/<tag>.jsonl
rx() {
  local tag=$1 st=0
  shift
  "$bin" --format cf32 "$@" <"$d/$tag.iq" >"$d/$tag.jsonl" 2>"$d/$tag.err" || st=$?
  [ "$st" = 0 ] || fail "$tag: onair-rx exited $st: $(tail -n 3 "$d/$tag.err")"
}
# lines <tag> <rate> <wanted members as JSON> [<exact line count>]: every line is one JSON object with a string event and an
# integer t_sample below the sample count; one line has the wanted members (a value {"has": [...]} wants a string with those
# substrings, and with "len": n one of n characters), and its t_sample lies within 0.5 s of where the generator put the message
lines() {
  python3 -I - "$d/$1.jsonl" "$d/$1.gen" "$2" "$(($(stat -c %s "$d/$1.iq") / 8))" "$3" "${4:-0}" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp, want, exact = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), json.loads(sys.argv[5]), int(sys.argv[6])
raw = open(path, "rb").read()
assert raw.endswith(b"\n") or not raw, "the last line is not ended"
lines = raw.splitlines()
assert lines, "no line"
assert not exact or len(lines) == exact, f"{len(lines)} lines, want {exact}"
g = re.search(r"from ([0-9.]+) s to ([0-9.]+) s", open(genlog).read())
assert g, "the generator did not say where the message is"
t0, t1 = float(g.group(1)), float(g.group(2))

def match(v, w):
    if isinstance(w, dict):
        return isinstance(v, str) and all(s in v for s in w["has"]) and len(v) == w.get("len", len(v))
    if isinstance(w, float):   # a whole number is written without a decimal point
        return isinstance(v, (int, float)) and not isinstance(v, bool) and abs(v - w) < 1e-3
    return type(v) is type(w) and v == w

found = None
for n, l in enumerate(lines):
    o = json.loads(l)
    assert isinstance(o, dict), f"line {n}: not an object"
    assert isinstance(o.get("event"), str), f"line {n}: no string event"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp, f"line {n}: t_sample {t!r} (of {nsamp} samples)"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    if found is None and all(match(o.get(k), w) for k, w in want.items()): found = o
assert found, "no line has " + ", ".join(f"{k}={w!r}" for k, w in want.items() if not isinstance(w, dict)) + f"; lines: {lines[:3]}"
t = found["t_sample"] / rate
assert t0 - 0.5 <= t <= t1 + 0.5, f"t_sample is {t:.3f} s, the message is at {t0:.3f} s to {t1:.3f} s"
print(f"ok: {len(lines)} line(s), {found['event']} at {t:.3f} s for a message at {t0:.3f} s to {t1:.3f} s")
EOF
}
# only <tag> <event>: every line is of that event: a mode writes only the bus event its row is for
only() {
  python3 -I - "$d/$1.jsonl" "$2" <<'EOF'
import json, sys
ev = [json.loads(l)["event"] for l in open(sys.argv[1], "rb")]
other = sorted(set(e for e in ev if e != sys.argv[2]))
assert not other, f"lines of {other}, not only {sys.argv[2]}"
print(f"ok: all {len(ev)} line(s) are {sys.argv[2]}")
EOF
}

# --- the known message of each mode
gen pocsag --mode pocsag --rate 1000000
rx pocsag --mode pocsag --rate 1000000 --freq "$acars_hz"   # --freq is taken and ignored
lines pocsag 1000000 '{"event": "pager.message", "protocol": "POCSAG", "address": 1234567, "function": 3, "text": "HELLO ZETA", "baud": 1200}' ||
  fail pocsag
only pocsag pager.message || fail pocsag

gen flex --mode flex --rate 1000000
rx flex --mode flex --rate 1000000
lines flex 1000000 '{"event": "pager.message", "protocol": "FLEX", "address": 1234567, "text": "HELLO ZETA"}' || fail flex
python3 -I -c '
import json, sys
o = [json.loads(l) for l in open(sys.argv[1])][0]
b, n = o.get("baud"), o.get("levels")
assert type(b) is int and n in (2, 4), "baud %r levels %r" % (b, n)
print("ok: FLEX baud %d, levels %d" % (b, n))' "$d/flex.jsonl" || fail flex
only flex pager.message || fail flex

gen acars --mode acars --rate 1000000 --freq "$acars_hz"
rx acars --mode acars --rate 1000000 --freq "$acars_hz"
lines acars 1000000 '{"event": "acars.message", "tail": "VH-ZET", "label": "H1", "flight": "ZET001"}' || fail acars
only acars acars.message || fail acars

gen aprs --mode aprs --rate 240000
rx aprs --mode aprs --rate 240000
lines aprs 240000 '{"event": "aprs.packet", "from": "VK3ABC", "to": "APRS", "info": "!2723.05S/15307.05E>ZetaSDR", "path": [], "fcs_ok": true, "lat": -27.38417, "lon": 153.1175}' ||
  fail aprs
only aprs aprs.packet || fail aprs
# an APRS message: one line, its aprs.packet (OnAir's bus also has an aprs.message for it, which is not the row's event)
gen aprs-msg --mode aprs --rate 240000 --info ':VK3XYZ   :hello{001'
rx aprs-msg --mode aprs --rate 240000
lines aprs-msg 240000 '{"event": "aprs.packet", "from": "VK3ABC", "to": "APRS", "info": ":VK3XYZ   :hello{001"}' 1 ||
  fail "an APRS message"
only aprs-msg aprs.packet || fail "an APRS message"
# an object's and a third-party packet's position is not the sender's: the line has no lat and no lon
# no_fix <tag>: no line has lat or lon
no_fix() {
  python3 -I -c '
import json, sys
ls = [json.loads(l) for l in open(sys.argv[1], "rb")]
bad = [o for o in ls if "lat" in o or "lon" in o]
assert not bad, f"a position on {bad[0]!r}"
print(f"ok: {len(ls)} line(s), none with lat or lon")' "$d/$1.jsonl"
}
gen aprs-object --mode aprs --rate 240000 --info ';LEADER   *092345z4903.50N/07201.75W>'
rx aprs-object --mode aprs --rate 240000
lines aprs-object 240000 '{"event": "aprs.packet", "from": "VK3ABC", "type": "Object", "info": ";LEADER   *092345z4903.50N/07201.75W>"}' 1 ||
  fail "an APRS object"
no_fix aprs-object || fail "an APRS object"
gen aprs-third --mode aprs --rate 240000 --info '}VK3XYZ>APRS,TCPIP,VK3ABC*:!2723.05S/15307.05E>inside'
rx aprs-third --mode aprs --rate 240000
lines aprs-third 240000 '{"event": "aprs.packet", "from": "VK3ABC", "info": {"has": ["}VK3XYZ>APRS"]}}' 1 ||
  fail "a third-party APRS packet"
no_fix aprs-third || fail "a third-party APRS packet"

# --- AIS: a position report (type 1) on AIS 1 and a static report (type 5, two sentences) on AIS 2, one line per message
# ais_details <tag>: the members a line of each message has, and its sentences
ais_details() {
  python3 -I - "$d/$1.jsonl" <<'EOF'
import json, sys
ls = [json.loads(l) for l in open(sys.argv[1], "rb")]
assert len(ls) == 2, f"{len(ls)} lines, want 2"
p, s = ls
assert p["msg_type"] == 1 and s["msg_type"] == 5, f"message types {p['msg_type']}, {s['msg_type']}"
assert abs(p["lat"] - -27.3842) < 1e-4 and abs(p["lon"] - 153.1175) < 1e-4, f"position {p.get('lat')}, {p.get('lon')}"
for o, n in ((p, 1), (s, 2)):
    nm = o["nmea"]
    assert isinstance(nm, list) and len(nm) == n, f"type {o['msg_type']}: nmea {nm!r}, want {n} sentences"
    assert all(isinstance(x, str) and x.startswith("!AIVDM,") and not x.endswith(("\r", "\n")) for x in nm), f"nmea {nm!r}"
assert "name" not in p and "lat" not in s and "sog_kt" not in s, "a member the message type does not carry"
print(f"ok: AIS lines {p['lat']}, {p['lon']} {p['sog_kt']} kt, and {s['name']!r} in {len(s['nmea'])} sentences")
EOF
}
gen ais --mode ais --rate 250000 --freq "$ais_hz"
rx ais --mode ais --rate 250000 --freq "$ais_hz"
lines ais 250000 '{"event": "ais.message", "msg_type": 1, "mmsi": 503000001, "channel": "A", "repeat": 0, "sog_kt": 12.3, "cog_deg": 45, "heading_deg": 45, "nav_status": 0}' 2 ||
  fail "ais: the position report"
lines ais 250000 '{"event": "ais.message", "msg_type": 5, "mmsi": 503000001, "channel": "B", "repeat": 0, "name": "ZETA TEST"}' 2 ||
  fail "ais: the static report"
ais_details ais || fail "ais"
only ais ais.message || fail "ais"
# centred on one channel, as the node runs it (a 25 kHz channel resampled up), in cs16: the channels are placed by frequency
for hz in 161975000 162025000; do
  gen "ais-$hz" --mode ais --rate 250000 --freq "$hz"
  python3 -I -c '
import struct, sys
a = struct.unpack(f"<{len(open(sys.argv[1], "rb").read()) // 4}f", open(sys.argv[1], "rb").read())
open(sys.argv[2], "wb").write(struct.pack(f"<{len(a)}h", *(max(-32768, min(32767, round(v * 32768))) for v in a)))' "$d/ais-$hz.iq" "$d/ais-$hz.cs16"
  "$bin" --mode ais --rate 250000 --format cs16 --freq "$hz" <"$d/ais-$hz.cs16" >"$d/ais-$hz.jsonl" 2>"$d/ais-$hz.err" ||
    fail "ais at $hz Hz: $(tail -n 3 "$d/ais-$hz.err")"
  lines "ais-$hz" 250000 '{"event": "ais.message", "msg_type": 1, "mmsi": 503000001, "channel": "A"}' 2 || fail "ais at $hz Hz"
  ais_details "ais-$hz" || fail "ais at $hz Hz"
  only "ais-$hz" ais.message || fail "ais at $hz Hz"
done

# --- VDL2: a downlink from aircraft 7C1234 to ground station 10A000, the station's uplink to it, the station's GSIF to all
# stations; icao is the source address and addr_type the source's type, as dumpvdl2 (the node's VDL2 row) has them
gen vdl2 --mode vdl2 --rate 250000
rx vdl2 --mode vdl2 --rate 250000 --freq "$vdl2_hz"
lines vdl2 250000 '{"event": "vdl2.message", "icao": "7C1234", "addr_type": "Aircraft", "from_air": true, "src": "7C1234", "dst": "10A000", "proto": "ACARS"}' 3 ||
  fail "vdl2: the downlink"
lines vdl2 250000 '{"event": "vdl2.message", "icao": "10A000", "addr_type": "Ground station", "from_air": false, "src": "10A000", "dst": "7C1234"}' 3 ||
  fail "vdl2: the uplink"
lines vdl2 250000 '{"event": "vdl2.message", "icao": "10A000", "addr_type": "Ground station", "from_air": false, "dst": "FFFFFF"}' 3 ||
  fail "vdl2: the GSIF"
only vdl2 vdl2.message || fail vdl2

# --- HFDL: on the dial frequency (the carrier 1440 Hz above it, as the receiver expects), ground station 1's squitter, then a
# logon request from aircraft 7C1234; the squitter's line has no icao (the bus has "" for none)
gen hfdl --mode hfdl --rate 24000
rx hfdl --mode hfdl --rate 24000 --freq "$hfdl_hz"
lines hfdl 24000 '{"event": "hfdl.message", "icao": "7C1234", "uplink": false}' 2 || fail "hfdl: the logon request"
python3 -I -c '
import json, sys
ls = [json.loads(l) for l in open(sys.argv[1], "rb")]
sq = [o for o in ls if "icao" not in o]
assert len(sq) == 1 and sq[0]["uplink"] is True, f"lines without icao: {sq!r}"
assert all(o.get("icao") != "" for o in ls), "an empty icao"
print("ok: the squitter (%s) has no icao" % sq[0]["kind"])' "$d/hfdl.jsonl" || fail "hfdl: the squitter"
only hfdl hfdl.message || fail hfdl

# --- the radiosondes, each its own type only: 12 s of one sonde at 0 Hz; serial is OnAir's, id the serial as rs1729's
# decoder for the type spells it (the node's rows key on it); the position within 0.001
for s in "rs41 RS41 S1234567 S1234567 -27.3842 153.1175" "dfm DFM 17012345 DFM-17012345 25.30 55.50" \
  "m20 M20 211-4-01234 M20-211-4-01234 25.00 55.60"; do
  read -r m type serial id lat lon <<<"$s"
  gen "$m" --mode "$m" --rate 2000000
  rx "$m" --mode "$m" --rate 2000000 --freq "$sonde_hz"
  lines "$m" 2000000 "{\"event\": \"sonde.update\", \"type\": \"$type\", \"id\": \"$id\", \"serial\": \"$serial\", \"lat\": $lat, \"lon\": $lon}" ||
    fail "$m"
  python3 -I -c '
import json, sys
ls = [json.loads(l) for l in open(sys.argv[1], "rb")]
bad = [o for o in ls if o.get("type") != sys.argv[2] or not o.get("id")]
assert not bad, f"lines of another type or with no id: {bad[:2]!r}"
print(f"ok: {len(ls)} line(s), all {sys.argv[2]} with an id")' "$d/$m.jsonl" "$type" || fail "$m"
  only "$m" sonde.update || fail "$m"
  # each sonde mode reports its own type only: the other two find the same sonde and write nothing
  for o in rs41 dfm m20; do
    [ "$o" != "$m" ] || continue
    "$bin" --mode "$o" --rate 2000000 --format cf32 <"$d/$m.iq" >"$d/$m-as-$o.jsonl" 2>"$d/$m-as-$o.err" ||
      fail "$m as $o: $(tail -n 3 "$d/$m-as-$o.err")"
    [ ! -s "$d/$m-as-$o.jsonl" ] || fail "$o wrote a line for the $type sonde: $(head -n 1 "$d/$m-as-$o.jsonl")"
  done
  echo "ok: rs41, dfm and m20 other than $m write nothing for the $type sonde"
  rm -f "$d/$m.iq"
done

# the joiner of multi-sentence messages on its own (onair-gen links onair-rx's)
"$gen" --selftest ais-join || fail "the AIS sentence joiner"
# a sonde's id on serials the test signals do not have (a short DFM-09 number, a DFM-06's hex, a long one)
"$gen" --selftest sonde-id || fail "the sonde id"

# --- text the bus cuts or that is not ASCII: one line, valid JSON
long=$(python3 -I -c 'import string; s = (string.ascii_letters + string.digits + " .,:;!?-+=/()\"\\") * 40; print("LONG " + s[:1995])')
[ "${#long}" = 2000 ] || fail "the long text is ${#long} characters"
gen pocsag-long --mode pocsag --rate 1000000 --text "$long"
rx pocsag-long --mode pocsag --rate 1000000
lines pocsag-long 1000000 '{"event": "pager.message", "address": 1234567, "text": {"has": ["LONG abcdefghij", "()\"\\"], "len": 2000}}' 1 ||
  fail "a POCSAG message of 2000 characters"
# 2100 characters: the bus keeps the first 2000, still one line
gen pocsag-longer --mode pocsag --rate 1000000 --text "${long}$(printf 'Z%.0s' $(seq 100))"
rx pocsag-longer --mode pocsag --rate 1000000
lines pocsag-longer 1000000 '{"event": "pager.message", "address": 1234567, "text": {"has": ["LONG abcdefghij", "()\"\\"], "len": 2000}}' 1 ||
  fail "a POCSAG message of 2100 characters"

odd=$'odd \x01 \x22 \x5c \xe9 end'
gen aprs-odd --mode aprs --rate 240000 --text "$odd"
rx aprs-odd --mode aprs --rate 240000
lines aprs-odd 240000 '{"event": "aprs.packet", "from": "VK3ABC", "info": {"has": ["odd \u0001 \" \\ é end"]}}' 1 ||
  fail "an APRS comment with bytes 0x01, 0x22, 0x5c and 0xe9"

# --- APRS packets on the pcap port; no client and a client that hangs up change neither the lines nor the run time
for i in 1 2 3 4 5 6; do gen "aprs-$i" --mode aprs --rate 240000 --text "message $i"; done
cat "$d"/aprs-[1-6].iq >"$d/aprs-many.iq"
python3 -I - "$bin" "$d/aprs.iq" "$d/aprs-many.iq" <<'EOF' || fail "the pcap port"
import socket, struct, subprocess, sys, threading, time
binary, one, many = sys.argv[1], sys.argv[2], sys.argv[3]

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

# the pcap client of test/run.sh: connects (retrying), then reads to EOF, or with hang_up closes after the 24-byte header
def client(port, hang_up, got, connected):
    deadline = time.time() + 10
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port)); break
        except OSError:
            if time.time() > deadline: return
            time.sleep(0.02)
    connected.set()
    buf = b""
    while True:
        b = s.recv(65536)
        if not b: break
        buf += b
        if hang_up and len(buf) >= 24: break
    s.close()
    got.append(buf)

# run: onair-rx --mode aprs on the file; how: "off" (no --pcap-port), "none" (no client), "all", "hang-up". The time is from
# the first byte written to the last line read: the decode loop's, without the pcap port's own shutdown at the end
def run(path, how):
    args = [binary, "--mode", "aprs", "--rate", "240000", "--format", "cf32"]
    port = free_port()
    if how != "off": args += ["--pcap-port", str(port)]
    p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    got, connected, th = [], threading.Event(), None
    if how in ("all", "hang-up"):
        th = threading.Thread(target=client, args=(port, how == "hang-up", got, connected)); th.start()
        assert connected.wait(10), "the client did not connect"
        if how == "hang-up":
            th.join(10)   # gone before the IQ flows
    data = open(path, "rb").read()
    out, last = [], [0.0]
    def read():
        for l in p.stdout:
            out.append(l); last[0] = time.monotonic()
    reader = threading.Thread(target=read); reader.start()
    t = time.monotonic()
    p.stdin.write(data); p.stdin.close()
    p.wait(60)
    reader.join()
    err = p.stderr.read().decode(errors="replace")
    assert p.returncode == 0, f"{how}: exit {p.returncode}: {err}"
    if th: th.join(10)
    return out, last[0] - t, (got[0] if got else None), err

# the frame on the port: a pcap header with link type 3 (AX.25) and one record holding the source address
lines, _, pcap, _ = run(one, "all")
assert pcap is not None and len(pcap) >= 24, "no pcap header"
magic, vmaj, vmin, _, _, _, link = struct.unpack("<IHHiIII", pcap[:24])
assert magic == 0xA1B2C3D4 and (vmaj, vmin) == (2, 4), "not a classic little-endian pcap header"
assert link == 3, f"link type {link}, not 3"
recs, i = [], 24
while i < len(pcap):
    sec, usec, incl, orig = struct.unpack("<IIII", pcap[i:i + 16])
    recs.append(pcap[i + 16:i + 16 + incl]); i += 16 + incl
assert i == len(pcap), "a cut record"
assert len(recs) == 1, f"{len(recs)} records, want 1"
assert bytes(c << 1 for c in b"VK3ABC") in recs[0], "the record does not hold VK3ABC as an AX.25 address"
assert len(lines) == 1, f"{len(lines)} lines with the pcap port"
print(f"ok: pcap link type 3, one record of {len(recs[0])} bytes holding VK3ABC")

# no client, and a client that hangs up after the header: the same lines, and the run time of a run without the option
best = {}
for how in ("off", "none", "hang-up") * 3:
    lines, took, _, err = run(many, how)
    n, t = best.get(how, (len(lines), took))
    assert len(lines) == n, f"{how}: {len(lines)} lines, then {n}"
    best[how] = (len(lines), min(t, took))
base_n, base_t = best["off"]
assert base_n == 6, f"{base_n} lines for 6 messages"
for how in ("none", "hang-up"):
    n, t = best[how]
    assert n == base_n, f"{how}: {n} lines, without the option {base_n}"
    assert abs(t - base_t) <= 0.2 * base_t, f"{how}: {t:.3f} s, without the option {base_t:.3f} s"
print("ok: " + ", ".join(f"{h} {best[h][0]} lines in {best[h][1]:.3f} s" for h in ("off", "none", "hang-up")))
EOF

# --- AIS on the pcap port: link type 101 (raw IP), one record per sentence, each the sentence with its line end in IPv4 + UDP
# to port 10110 (NMEA 0183), as OnAir's capture files have it; the lines are those of a run without the port
python3 -I - "$bin" "$d/ais.iq" "$d/ais.jsonl" "$ais_hz" <<'EOF' || fail "AIS on the pcap port"
import json, socket, struct, subprocess, sys, threading, time
binary, iq, plain, hz = sys.argv[1:5]
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
p = subprocess.Popen([binary, "--mode", "ais", "--rate", "250000", "--format", "cf32", "--freq", hz, "--pcap-port", str(port)],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
deadline = time.time() + 10
while True:
    try:
        c = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        assert time.time() < deadline, "the client did not connect"
        time.sleep(0.02)
got = []
def read():
    buf = b""
    while (b := c.recv(65536)): buf += b
    got.append(buf)
th = threading.Thread(target=read); th.start()
out, err = p.communicate(open(iq, "rb").read(), timeout=60)
th.join(10)
assert p.returncode == 0, f"exit {p.returncode}: {err.decode(errors='replace')}"
def members(raw):   # t_sample follows how the pipe hands over the input: the rest is compared
    return [{k: v for k, v in json.loads(l).items() if k != "t_sample"} for l in raw.splitlines()]
assert members(out) == members(open(plain, "rb").read()), "the lines differ from a run without the port"
pcap = got[0]
magic, vmaj, vmin, _, _, _, link = struct.unpack("<IHHiIII", pcap[:24])
assert magic == 0xA1B2C3D4 and (vmaj, vmin) == (2, 4), "not a classic little-endian pcap header"
assert link == 101, f"link type {link}, not 101"
recs, i = [], 24
while i < len(pcap):
    _, _, incl, _ = struct.unpack("<IIII", pcap[i:i + 16])
    recs.append(pcap[i + 16:i + 16 + incl]); i += 16 + incl
assert i == len(pcap), "a cut record"
want = [x.encode() + b"\r\n" for l in out.splitlines() for x in json.loads(l)["nmea"]]
assert len(recs) == 3 == len(want), f"{len(recs)} records, want 3 ({len(want)} sentences on the lines)"
for r, w in zip(recs, want):
    assert r[0] == 0x45 and r[9] == 17, "not IPv4 + UDP"
    ihl = (r[0] & 15) * 4
    total, = struct.unpack(">H", r[2:4])
    dport, ulen = struct.unpack(">HH", r[ihl + 2:ihl + 6])
    assert total == len(r) and ulen == len(r) - ihl, "the lengths do not fit the record"
    assert dport == 10110, f"UDP port {dport}, not 10110"
    assert r[ihl + 8:] == w, f"payload {r[ihl + 8:]!r}, want {w!r}"
print(f"ok: pcap link type 101, {len(recs)} records, each a line's sentence in IPv4 + UDP to port 10110")
EOF
echo "PASS"
