#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the bus modes (pocsag, flex, acars, aprs) on OnAir's own test signals: _build/onair-gen makes one known
# message per mode with a second of noise either side, onair-rx decodes it. The signals are synthesised; no capture is used.
# Also: a mode that needs --freq refuses to start without it, text the bus cuts or that is not ASCII stays one valid line, and
# a pcap port with no client, or with a client that hangs up, changes neither the lines nor the run time.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
gen=${ONAIR_GEN:-$here/_build/onair-gen}
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
acars_hz=131525000

# --- onair-rx has each mode: on empty input it ends with exit 0 and no line
for m in pocsag flex acars aprs; do
  f=()
  [ "$m" != acars ] || f=(--freq "$acars_hz")
  "$bin" --mode "$m" --rate 1000000 --format cf32 "${f[@]}" </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" ||
    fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has pocsag, flex, acars and aprs"

# --- acars without --freq: exit 2 with one line, before reading any input (stdin stays open and is never written)
python3 -I - "$bin" <<'EOF' || fail "acars without --freq"
import subprocess, sys
p = subprocess.Popen([sys.argv[1], "--mode", "acars", "--rate", "1000000"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
    sys.exit("acars without --freq waited for input")
out, err = p.stdout.read(), p.stderr.read()
assert p.returncode == 2, f"exit {p.returncode}, not 2"
assert err == b"onair-rx: acars needs --freq HZ\n", f"stderr {err!r}"
assert out == b"", "it wrote to stdout"
print("ok: acars without --freq exits 2 before reading input: onair-rx: acars needs --freq HZ")
EOF

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
# integer t_sample below the sample count; one line has the wanted members (a value {"has": [...]} wants those substrings),
# and its t_sample lies within 0.5 s of where the generator put the message
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
        return isinstance(v, str) and all(s in v for s in w["has"])
    if isinstance(w, float):
        return isinstance(v, float) and abs(v - w) < 1e-3
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

# --- the known message of each mode
gen pocsag --mode pocsag --rate 1000000
rx pocsag --mode pocsag --rate 1000000 --freq "$acars_hz"   # --freq is taken and ignored
lines pocsag 1000000 '{"event": "pager.message", "protocol": "POCSAG", "address": 1234567, "function": 3, "text": "HELLO ZETA", "baud": 1200}' ||
  fail pocsag

gen flex --mode flex --rate 1000000
rx flex --mode flex --rate 1000000
lines flex 1000000 '{"event": "pager.message", "protocol": "FLEX", "address": 1234567, "text": "HELLO ZETA"}' || fail flex
python3 -I -c '
import json, sys
o = [json.loads(l) for l in open(sys.argv[1])][0]
b, n = o.get("baud"), o.get("levels")
assert type(b) is int and n in (2, 4), "baud %r levels %r" % (b, n)
print("ok: FLEX baud %d, levels %d" % (b, n))' "$d/flex.jsonl" || fail flex

gen acars --mode acars --rate 1000000 --freq "$acars_hz"
rx acars --mode acars --rate 1000000 --freq "$acars_hz"
lines acars 1000000 '{"event": "acars.message", "tail": "VH-ZET", "label": "H1", "flight": "ZET001"}' || fail acars

gen aprs --mode aprs --rate 240000
rx aprs --mode aprs --rate 240000
lines aprs 240000 '{"event": "aprs.packet", "from": "VK3ABC", "to": "APRS", "info": "!2723.05S/15307.05E>ZetaSDR", "path": [], "fcs_ok": true, "lat": -27.38417, "lon": 153.1175}' ||
  fail aprs

# --- text the bus cuts or that is not ASCII: one line, valid JSON
long=$(python3 -I -c 'import string; s = (string.ascii_letters + string.digits + " .,:;!?-+=/()\"\\") * 40; print("LONG " + s[:1995])')
[ "${#long}" = 2000 ] || fail "the long text is ${#long} characters"
gen pocsag-long --mode pocsag --rate 1000000 --text "$long"
rx pocsag-long --mode pocsag --rate 1000000
lines pocsag-long 1000000 '{"event": "pager.message", "address": 1234567, "text": {"has": ["LONG abcdefghij", "()\"\\"]}}' 1 ||
  fail "a POCSAG message of 2000 characters"

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
echo "PASS"
