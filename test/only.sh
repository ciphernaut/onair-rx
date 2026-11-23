#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the modes only OnAir decodes (so far ble and lorawan) on OnAir's own test signals: _build/onair-gen makes
# the scene of OnAir's generator for the mode (makeBleSynth, makeLorawanSynth) with a second of noise either side, onair-rx
# decodes it. The signals are synthesised; no capture is used. Also: the options a mode does not take (--width, --plan,
# --region, --drop) are refused by every mode, lorawan without --freq and at a --freq no channel plan covers, the frames on the
# pcap port, and onair-gen's --sigmf pair read back.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
gen=${ONAIR_GEN:-$here/_build/onair-gen}
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
ble_hz=2402000000    # advertising channel 37
lora_hz=868300000    # the centre of OnAir's EU868 scene (lorawanPlanCentreMhz)

# --- onair-rx has each mode: on empty input it ends with exit 0 and no line
"$bin" --mode ble --rate 4000000 </dev/null >"$d/ble.empty" 2>"$d/ble.empty.err" || fail "ble: $(cat "$d/ble.empty.err")"
[ ! -s "$d/ble.empty" ] || fail "ble: a line on empty input"
"$bin" --mode lorawan --rate 2000000 --freq "$lora_hz" </dev/null >"$d/lorawan.empty" 2>"$d/lorawan.empty.err" ||
  fail "lorawan: $(cat "$d/lorawan.empty.err")"
[ ! -s "$d/lorawan.empty" ] || fail "lorawan: a line on empty input"
echo "ok: onair-rx has ble and lorawan"

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
refuses "onair-rx: lorawan needs --freq HZ" --mode lorawan --rate 2000000 || fail "lorawan without --freq"
refuses "onair-rx: lorawan: no channel plan has a channel within 99.000-101.000 MHz" --mode lorawan --rate 2000000 \
  --freq 100000000 || fail "lorawan at a --freq no plan covers"
refuses "onair-rx: lorawan: plan US915 has no channel within 867.300-869.300 MHz" --mode lorawan --rate 2000000 \
  --freq "$lora_hz" --plan US915 || fail "lorawan with a --plan that has no channel in the input"
refuses "onair-rx: lorawan: no plan XX868 (EU868, US915, AU915, AS923-1, AS923-2, AS923-3, AS923-4, EU433)" --mode lorawan \
  --rate 2000000 --freq "$lora_hz" --plan XX868 || fail "lorawan with an unknown --plan"
refuses "onair-rx: 2000000 Hz is too low for ble (at least 4000000 Hz)" --mode ble --rate 2000000 || fail "ble at 2 MHz"
refuses "onair-rx: bad --width 0" --mode ble --rate 4000000 --width 0 || fail "--width 0"

# --- the options a mode does not take: every mode before this plan refuses all four; ble all four; lorawan all but --plan.
# Each mode gets the rate and the --freq it needs, so that the option is the only thing wrong
for row in "adsb 2400000" "pocsag 1000000" "flex 1000000" "acars 1000000 131525000" "aprs 240000" "ais 250000 162000000" \
  "vdl2 250000 136975000" "hfdl 24000 8927000" "rs41 2000000" "dfm 2000000" "m20 2000000" "rds 500000" "dmr 48000" \
  "p25 250000" "m17 250000" "dvbt 10000000" "video 8000000" "ble 4000000" "lorawan 2000000 $lora_hz"; do
  read -r m rate hz <<<"$row"
  f=(--mode "$m" --rate "$rate")
  [ -z "${hz:-}" ] || f+=(--freq "$hz")
  for opt in "width 2000000" "plan EU868" "region EU" "drop"; do
    read -r o v <<<"$opt"
    [ "$m/$o" != lorawan/plan ] || continue
    a=(--"$o")
    [ -z "${v:-}" ] || a+=("$v")
    refuses "onair-rx: $m takes no --$o" "${f[@]}" "${a[@]}" >/dev/null || fail "$m with --$o"
  done
done
echo "ok: every mode refuses the options it does not take (lorawan takes --plan)"

[ -x "$gen" ] || fail "no test signal generator at $gen (run ./build.sh)"

# gen <tag> <onair-gen args...>: the IQ in $d/<tag>.iq, what the generator said (where the message is) in $d/<tag>.gen
gen() {
  local tag=$1
  shift
  "$gen" "$@" >"$d/$tag.iq" 2>"$d/$tag.gen" || fail "onair-gen $*: $(cat "$d/$tag.gen")"
}
# rx <tag> <onair-rx args...>: $d/<tag>.iq through onair-rx into $d/<tag>.jsonl, its stderr in $d/<tag>.err
rx() {
  local tag=$1 st=0
  shift
  "$bin" --format cf32 "$@" <"$d/$tag.iq" >"$d/$tag.jsonl" 2>"$d/$tag.err" || st=$?
  [ "$st" = 0 ] || fail "$tag: onair-rx exited $st: $(tail -n 3 "$d/$tag.err")"
}
# lines <jsonl> <gen log> <rate> <samples> <check>: every line one JSON object with the string event of the check, an integer
# t_sample below the sample count that never goes down and within 0.5 s of the generator's message, no null member; then the
# check's own members (ble: the advertisers of OnAir's scene; lorawan: the frames of OnAir's EU868 scene)
lines() {
  python3 -I - "$@" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp, check = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
raw = open(path, "rb").read()
assert raw.endswith(b"\n") or not raw, "the last line is not ended"
ls = [json.loads(l) for l in raw.splitlines()]
assert ls, "no line"
g = re.search(r"from ([0-9.]+) s to ([0-9.]+) s", open(genlog).read())
assert g, "the generator did not say where the message is"
t0, t1 = float(g.group(1)), float(g.group(2))
last = 0
for n, o in enumerate(ls):
    assert isinstance(o, dict), f"line {n}: not an object"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp, f"line {n}: t_sample {t!r} (of {nsamp} samples)"
    assert t >= last, f"line {n}: t_sample {t} after {last}"
    last = t
    assert t0 - 0.5 <= t / rate <= t1 + 0.5, f"line {n}: at {t / rate:.3f} s, the message is at {t0:.3f} s to {t1:.3f} s"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
if check == "ble":
    # the advertisers of OnAir's scene (ble_gen.cpp, bleTestSchedule): a phone, a beacon, an Eddystone, a resolvable address
    # and the scanner that sends the scan request and the connection request
    scene = {"c0ffee123456", "000ba15e0001", "3a5544332202", "5b123456789a", "665544332211"}
    for n, o in enumerate(ls):
        assert o.get("event") == "ble.device", f"line {n}: event {o.get('event')!r}"
        mac = o.get("mac")
        assert isinstance(mac, str) and re.fullmatch(r"[0-9a-f]{12}", mac), f"line {n}: mac {mac!r}"
        assert mac in scene, f"line {n}: mac {mac} is none of the scene's devices"
        assert isinstance(o.get("rssi_db"), (int, float)), f"line {n}: rssi_db {o.get('rssi_db')!r}"
        assert o.get("name", "x") != "", f"line {n}: an empty name"
    names = {o["name"] for o in ls if o["mac"] == "c0ffee123456" and "name" in o}
    assert names & {"OnAir", "OnAir test phone"}, f"the phone c0ffee123456 is not named: {names!r}"
    print(f"ok: {len(ls)} ble.device line(s), devices {sorted({o['mac'] for o in ls})}")
else:
    lo, hi = float(sys.argv[6]) - rate / 2, float(sys.argv[6]) + rate / 2
    for n, o in enumerate(ls):
        assert o.get("event") == "lora.packet", f"line {n}: event {o.get('event')!r}"
        for k in ("sf", "freq_hz"):
            assert type(o.get(k)) is int, f"line {n}: {k} {o.get(k)!r}"
        assert lo <= o["freq_hz"] <= hi, f"line {n}: freq_hz {o['freq_hz']} outside {lo:.0f} to {hi:.0f} Hz"
        assert o.get("kind") == "lorawan" and o.get("modulation") in ("lora", "fsk"), f"line {n}: {o!r}"
        # the PHY payload as OnAir's bus has it: upper-case byte pairs, a space between
        assert isinstance(o.get("hex"), str) and re.fullmatch(r"[0-9A-F]{2}( [0-9A-F]{2})*", o["hex"]), f"line {n}: hex {o.get('hex')!r}"
        assert "devaddr" not in o or re.fullmatch(r"[0-9a-f]{8}", o["devaddr"]), f"line {n}: devaddr {o['devaddr']!r}"
    addrs = {o.get("devaddr") for o in ls}
    # two uplinks of OnAir's scene (lorawan_gen.cpp, lorawanTestSchedule): DevAddr 26011A2B on SF7 and 26014C01 on SF11
    want = {"26011a2b", "26014c01"}
    assert want <= addrs, f"devaddrs {sorted(a for a in addrs if a)}, want {sorted(want)} among them"
    print(f"ok: {len(ls)} lora.packet line(s), SF {sorted({o['sf'] for o in ls})}, "
          f"{len({o['freq_hz'] for o in ls})} channel(s), devaddrs {sorted(a for a in addrs if a)}")
EOF
}
samples() { echo $(($(stat -c %s "$d/$1.iq") / 8)); }

# --- Bluetooth LE: OnAir's advertisers on the channel at 0 Hz, with --freq naming channel 37 and without it
gen ble --mode ble --rate 4000000
rx ble --mode ble --rate 4000000 --freq "$ble_hz"
lines "$d/ble.jsonl" "$d/ble.gen" 4000000 "$(samples ble)" ble || fail ble
"$bin" --mode ble --rate 4000000 <"$d/ble.iq" >"$d/ble-nofreq.jsonl" 2>"$d/ble-nofreq.err" || fail "ble without --freq"
lines "$d/ble-nofreq.jsonl" "$d/ble.gen" 4000000 "$(samples ble)" ble || fail "ble without --freq"

# --- LoRaWAN: OnAir's EU868 scene, the plan found from --freq and named by --plan
gen lorawan --mode lorawan --rate 2000000 --freq "$lora_hz"
rx lorawan --mode lorawan --rate 2000000 --freq "$lora_hz"
lines "$d/lorawan.jsonl" "$d/lorawan.gen" 2000000 "$(samples lorawan)" lorawan "$lora_hz" || fail lorawan
rx lorawan --mode lorawan --rate 2000000 --freq "$lora_hz" --plan EU868
mv "$d/lorawan.jsonl" "$d/lorawan-plan.jsonl"
"$bin" --mode lorawan --rate 2000000 --freq "$lora_hz" <"$d/lorawan.iq" >"$d/lorawan.jsonl" 2>/dev/null
python3 -I -c '
import json, sys
strip = lambda p: [{k: v for k, v in json.loads(l).items() if k != "t_sample"} for l in open(p, "rb")]
assert strip(sys.argv[1]) == strip(sys.argv[2]), "--plan EU868 gives other lines than the plan found"
print("ok: --plan EU868 gives the lines of the plan found from --freq")' "$d/lorawan.jsonl" "$d/lorawan-plan.jsonl" ||
  fail "lorawan with --plan"

# --- the frames on the pcap port: the file header's link type, at least one record of the channel's own form, and the lines
# of a run without the port. pcap <tag> <link type> <onair-rx args...>
pcap() {
  python3 -I - "$bin" "$d/$1.iq" "$d/$1.jsonl" "$@" <<'EOF'
import json, socket, struct, subprocess, sys, threading, time
binary, iq, plain, tag, link_want, args = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), sys.argv[6:]
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
p = subprocess.Popen([binary, "--format", "cf32", *args, "--pcap-port", str(port)],
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
out, err = p.communicate(open(iq, "rb").read(), timeout=120)
th.join(10)
assert p.returncode == 0, f"exit {p.returncode}: {err.decode(errors='replace')}"
members = lambda raw: [{k: v for k, v in json.loads(l).items() if k != "t_sample"} for l in raw.splitlines()]
assert members(out) == members(open(plain, "rb").read()), "the lines differ from a run without the port"
pcap = got[0]
magic, vmaj, vmin, _, _, _, link = struct.unpack("<IHHiIII", pcap[:24])
assert magic == 0xA1B2C3D4 and (vmaj, vmin) == (2, 4), "not a classic little-endian pcap header"
assert link == link_want, f"link type {link}, not {link_want}"
recs, i = [], 24
while i < len(pcap):
    _, _, incl, _ = struct.unpack("<IIII", pcap[i:i + 16])
    recs.append(pcap[i + 16:i + 16 + incl]); i += 16 + incl
assert i == len(pcap), "a cut record"
assert recs, "no record"
if tag == "ble":   # BLUETOOTH_LE_LL: the advertising access address 0x8E89BED6, little endian, then the PDU
    assert all(r[:4] == bytes.fromhex("d6be898e") for r in recs), "a record without the advertising access address"
else:              # LoRaTap version 0, its 15-byte header, the channel's frequency inside the input
    hz, rate = float(args[args.index("--freq") + 1]), float(args[args.index("--rate") + 1])
    for r in recs:
        assert r[0] == 0 and struct.unpack(">H", r[2:4])[0] == 15, "not a LoRaTap version 0 header"
        f, = struct.unpack(">I", r[4:8])
        assert hz - rate / 2 <= f <= hz + rate / 2, f"a record at {f} Hz, outside the input"
print(f"ok: {tag}: pcap link type {link}, {len(recs)} record(s), the lines of a run without the port")
EOF
}
pcap ble 251 --mode ble --rate 4000000 --freq "$ble_hz" || fail "ble on the pcap port"
pcap lorawan 270 --mode lorawan --rate 2000000 --freq "$lora_hz" || fail "lorawan on the pcap port"

# --- onair-gen --sigmf: the pair (ci16_le) holds the signal of stdout, and onair-rx decodes it as cs16
"$gen" --mode ble --rate 4000000 --sigmf "$d/ble-pair" >"$d/ble-pair.out" 2>"$d/ble-pair.gen" ||
  fail "onair-gen --sigmf: $(cat "$d/ble-pair.gen")"
[ ! -s "$d/ble-pair.out" ] || fail "onair-gen --sigmf wrote to stdout"
python3 -I - "$d/ble-pair" "$d/ble.iq" "$ble_hz" <<'EOF' || fail "the --sigmf pair"
import json, struct, sys
stem, iq, hz = sys.argv[1], sys.argv[2], int(sys.argv[3])
m = json.load(open(stem + ".sigmf-meta"))
g = m["global"]
assert g["core:datatype"] == "ci16_le" and g["core:sample_rate"] == 4000000 and g["core:version"] == "1.0.0", f"global {g!r}"
assert m["captures"] == [{"core:frequency": hz, "core:sample_start": 0}], f"captures {m['captures']!r}"
assert isinstance(m.get("annotations"), list), "no annotations"
data, ref = open(stem + ".sigmf-data", "rb").read(), open(iq, "rb").read()
assert len(data) * 2 == len(ref), f"{len(data)} bytes of ci16 for {len(ref)} bytes of cf32"
n = len(ref) // 4
a, b = struct.unpack(f"<{n}h", data), struct.unpack(f"<{n}f", ref)
assert all(abs(x - max(-32768, min(32767, round(y * 32768)))) <= 1 for x, y in zip(a[:200000], b[:200000])), "not the stdout signal"
print(f"ok: the --sigmf pair: {n // 2} samples of ci16_le at 4000000 Hz, centred on {hz} Hz")
EOF
"$bin" --mode ble --rate 4000000 --format cs16 <"$d/ble-pair.sigmf-data" >"$d/ble-pair.jsonl" 2>"$d/ble-pair.err" ||
  fail "ble on the pair: $(tail -n 3 "$d/ble-pair.err")"
lines "$d/ble-pair.jsonl" "$d/ble.gen" 4000000 $(($(stat -c %s "$d/ble-pair.sigmf-data") / 4)) ble ||
  fail "ble on the --sigmf pair"
# a mode the generator has no centre for (pocsag, no --freq) still gets its pair, without a frequency in the capture
"$gen" --mode pocsag --rate 1000000 --sigmf "$d/pocsag-pair" 2>/dev/null || fail "onair-gen --mode pocsag --sigmf"
python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1] + ".sigmf-meta"))
assert m["global"]["core:datatype"] == "ci16_le" and m["global"]["core:sample_rate"] == 1000000, m["global"]
assert m["captures"] == [{"core:sample_start": 0}], m["captures"]
print("ok: a pocsag pair without a centre frequency")' "$d/pocsag-pair" || fail "the pocsag --sigmf pair"
echo "PASS"
