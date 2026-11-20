#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# End-to-end check of onair-rx on a recorded 1090 MHz capture (iq/adsb1090.sigmf-data: cf32, 2457600 Hz).
# The capture holds real traffic, so this script prints counts only, never an address or a message.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
iq=${ONAIR_RX_IQ:-$here/iq/adsb1090.sigmf-data}
rate=2457600
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

# --- an unknown mode: exit 2 and the one line
set +e
"$bin" --mode nope --rate "$rate" --format cf32 </dev/null >"$d/nope.out" 2>"$d/nope.err"
st=$?
set -e
[ "$st" = 2 ] || fail "--mode nope exited $st, not 2"
grep -qx 'onair-rx: no mode nope' "$d/nope.err" || fail "--mode nope said: $(cat "$d/nope.err")"
[ ! -s "$d/nope.out" ] || fail "--mode nope wrote to stdout"
echo "ok: --mode nope exits 2"

# --- --version: the attribution line
"$bin" --version >"$d/version"
grep -qx 'Based on OnAir by Kristijonas Adomaitis - https://on-air.dev' "$d/version" || fail "--version lacks the attribution"
echo "ok: --version carries the attribution"

if [ ! -f "$iq" ]; then
  echo "skip: no capture at $iq (the decode checks need it)"
  exit 0
fi

free_port() { python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

# A pcap client: connects (retrying), marks the connection, reads to EOF into a file. With "hang-up" it closes
# right after the 24-byte file header.
client=$d/client.py
cat >"$client" <<'EOF'
import socket, sys, time, pathlib
port, out, mark, how = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
deadline = time.time() + 10
while True:
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        if time.time() > deadline: sys.exit("client: no listener")
        time.sleep(0.05)
pathlib.Path(mark).touch()
buf = b""
while True:
    b = s.recv(65536)
    if not b: break
    buf += b
    if how == "hang-up" and len(buf) >= 24: break
s.close()
pathlib.Path(out).write_bytes(buf)
EOF

# run <format> <input file> <client mode> <tag>: onair-rx with a pcap client attached before the IQ flows
run() {
  local fmt=$1 input=$2 how=$3 tag=$4 port
  port=$(free_port)
  python3 -I "$client" "$port" "$d/$tag.pcap" "$d/$tag.connected" "$how" &
  local cpid=$!
  set +e
  ( for _ in $(seq 200); do [ -e "$d/$tag.connected" ] && break; sleep 0.05; done; cat "$input" ) |
    "$bin" --mode adsb --rate "$rate" --format "$fmt" --pcap-port "$port" >"$d/$tag.jsonl" 2>"$d/$tag.err"
  local st=$?
  set -e
  wait "$cpid" || fail "$tag: the pcap client failed"
  [ "$st" = 0 ] || fail "$tag: onair-rx exited $st: $(tail -n 3 "$d/$tag.err")"
}

# check <tag> <with pcap: 1|0>: the JSON lines and, when asked, the pcap stream
check() {
  python3 -I - "$d/$1.jsonl" "$d/$1.pcap" "$2" <<'EOF'
import json, re, struct, sys
lines = open(sys.argv[1]).read().splitlines()
assert len(lines) >= 150, f"{len(lines)} JSON lines, want at least 150"
known = {"icao", "df", "tc", "level_dbfs", "msg", "callsign", "altitude_ft", "lat", "lon",
         "ground_speed_kt", "track_deg", "squawk", "on_ground"}
alt = msgs = pos = 0
for n, l in enumerate(lines):
    o = json.loads(l)
    assert isinstance(o, dict), f"line {n}: not an object"
    assert re.fullmatch(r"[0-9A-F]{6}", o.get("icao", "")), f"line {n}: icao is not 6 upper-case hex digits"
    assert set(o) <= known, f"line {n}: unknown members {sorted(set(o) - known)}"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert isinstance(o["df"], int) and isinstance(o["level_dbfs"], (int, float)), f"line {n}: df / level_dbfs"
    if "msg" in o: assert o["msg"] in range(1, 9), f"line {n}: msg {o['msg']}"; msgs += 1
    if "squawk" in o: assert re.fullmatch(r"[0-7]{4}", o["squawk"]), f"line {n}: squawk"
    if "altitude_ft" in o: assert isinstance(o["altitude_ft"], int); alt += 1
    if "lat" in o: assert "lon" in o; pos += 1
assert alt >= 1, "no line has altitude_ft"
print(f"ok: {len(lines)} JSON lines, {msgs} with msg, {alt} with altitude_ft, {pos} with a position")
if sys.argv[3] == "1":
    b = open(sys.argv[2], "rb").read()
    assert len(b) >= 24, "no pcap header"
    magic, vmaj, vmin, _, _, snap, link = struct.unpack("<IHHiIII", b[:24])
    assert magic == 0xA1B2C3D4 and (vmaj, vmin) == (2, 4), "not a classic little-endian pcap header"
    assert link == 147, f"link type {link}, not 147"
    i, recs, last = 24, 0, (0, 0)
    while i < len(b):
        assert i + 16 <= len(b), "a cut record header"
        sec, usec, incl, orig = struct.unpack("<IIII", b[i:i + 16])
        assert incl == orig and incl in (7, 14), f"record {recs}: {incl} bytes"
        assert usec < 1000000 and (sec, usec) >= last, f"record {recs}: time"
        last = (sec, usec)
        i += 16 + incl
        assert i <= len(b), "a cut record"
        recs += 1
    assert recs >= 150, f"{recs} pcap records, want at least 150"
    assert recs == len(lines), f"{recs} pcap records for {len(lines)} JSON lines"
    print(f"ok: pcap link type 147, {recs} records (7 or 14 bytes each)")
EOF
}

run cf32 "$iq" all cf32
check cf32 1 || fail "cf32 run"

# a client that hangs up after the header must not stop the decoding
run cf32 "$iq" hang-up hangup
check hangup 0 || fail "hang-up run"
echo "ok: a client hanging up does not stop onair-rx"

# the node's format: cs16 (needs numpy for the conversion)
if python3 -I -c 'import numpy' 2>/dev/null; then
  python3 -I -c '
import numpy as np, sys
x = np.fromfile(sys.argv[1], dtype="<f4")
np.clip(np.round(x * 32767), -32768, 32767).astype("<i2").tofile(sys.argv[2])' "$iq" "$d/iq.cs16"
  run cs16 "$d/iq.cs16" all cs16
  check cs16 1 || fail "cs16 run"
else
  echo "skip: no numpy for the cs16 run"
fi
echo "PASS"
