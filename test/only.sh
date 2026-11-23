#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the modes only OnAir decodes (so far ble, lorawan, zigbee, thread, zwave and wifi) on OnAir's own test
# signals: _build/onair-gen makes the scene of OnAir's generator for the mode (makeBleSynth, makeLorawanSynth, makeZigbeeSynth,
# the frames of OnAir's Thread test, makeZwaveSynth, makeWifiSynth) with a second of noise either side (Wi-Fi: a tenth of a
# second, at 20 Msps), onair-rx decodes it. The four modes whose lines come from their Wireshark hub packets (zigbee, thread,
# zwave, wifi) are also checked against what OnAir's generator put in the scene, their pcap link types, --freq missing, and
# noise alone (8 s of /dev/urandom: no line, exit 0); the script prints each mode's user CPU over its seconds of input. The signals are synthesised; no capture is used. Also: the options a mode does not take (--width, --plan,
# --region, --drop) are refused by every mode that does not take them, lorawan without --freq and at a --freq no channel plan covers, the frames on the
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
zb_hz=2425000000     # 802.15.4 channel 15
zw_hz=869130000      # the centre of OnAir's EU Z-Wave scene (zwave::regions())
wf_hz=2437000000     # Wi-Fi channel 6
for row in "zigbee 4000000 $zb_hz" "thread 4000000 $zb_hz" "zwave 2000000 $zw_hz" "wifi 20000000 $wf_hz"; do
  read -r m rate hz <<<"$row"
  "$bin" --mode "$m" --rate "$rate" --freq "$hz" </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" || fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has zigbee, thread, zwave and wifi"

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
refuses "onair-rx: zwave: no region XX (EU, US, ANZ, HK, IN, RU, CN, IL, JP, KR)" --mode zwave --rate 2000000 --freq "$zw_hz" \
  --region XX || fail "zwave with an unknown --region"
refuses "onair-rx: zwave: region US has no channel within 868.230-870.030 MHz" --mode zwave --rate 2000000 --freq "$zw_hz" \
  --region US || fail "zwave with a --region that has no channel in the input"
refuses "onair-rx: zwave: no region has a channel within 99.100-100.900 MHz" --mode zwave --rate 2000000 --freq 100000000 ||
  fail "zwave at a --freq no region covers"
refuses "onair-rx: zigbee needs --freq HZ" --mode zigbee --rate 4000000 || fail "zigbee without --freq"
refuses "onair-rx: thread needs --freq HZ" --mode thread --rate 4000000 || fail "thread without --freq"
refuses "onair-rx: zwave needs --freq HZ" --mode zwave --rate 2000000 || fail "zwave without --freq"
refuses "onair-rx: wifi needs --freq HZ" --mode wifi --rate 20000000 || fail "wifi without --freq"
refuses "onair-rx: 2000000 Hz is too low for zigbee (at least 4000000 Hz)" --mode zigbee --rate 2000000 --freq "$zb_hz" ||
  fail "zigbee at 2 MHz"
refuses "onair-rx: 4000000 Hz is too low for wifi (at least 20000000 Hz)" --mode wifi --rate 4000000 --freq "$wf_hz" ||
  fail "wifi at 4 MHz"

# --- the options a mode does not take: every mode before this plan refuses all four; ble, zigbee, thread and wifi all four;
# lorawan all but --plan; zwave all but --region.
# Each mode gets the rate and the --freq it needs, so that the option is the only thing wrong
for row in "adsb 2400000" "pocsag 1000000" "flex 1000000" "acars 1000000 131525000" "aprs 240000" "ais 250000 162000000" \
  "vdl2 250000 136975000" "hfdl 24000 8927000" "rs41 2000000" "dfm 2000000" "m20 2000000" "rds 500000" "dmr 48000" \
  "p25 250000" "m17 250000" "dvbt 10000000" "video 8000000" "ble 4000000" "lorawan 2000000 $lora_hz" \
  "zigbee 4000000 $zb_hz" "thread 4000000 $zb_hz" "zwave 2000000 $zw_hz" "wifi 20000000 $wf_hz"; do
  read -r m rate hz <<<"$row"
  f=(--mode "$m" --rate "$rate")
  [ -z "${hz:-}" ] || f+=(--freq "$hz")
  for opt in "width 2000000" "plan EU868" "region EU" "drop"; do
    read -r o v <<<"$opt"
    case "$m/$o" in lorawan/plan | zwave/region) continue ;; esac
    a=(--"$o")
    [ -z "${v:-}" ] || a+=("$v")
    refuses "onair-rx: $m takes no --$o" "${f[@]}" "${a[@]}" >/dev/null || fail "$m with --$o"
  done
done
echo "ok: every mode refuses the options it does not take (lorawan takes --plan, zwave --region)"

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
assert len(recs) >= len(out.splitlines()), f"{len(recs)} record(s) for {len(out.splitlines())} line(s)"
def kermit(b):   # IEEE 802.15.4's FCS: CRC-16 of x^16 + x^12 + x^5 + 1, initial value 0, least significant bit first
    c = 0
    for x in b:
        c ^= x
        for _ in range(8): c = (c >> 1) ^ (0x8408 if c & 1 else 0)
    return c
if tag in ("zigbee", "thread"):   # the PSDU as received, its FCS in the last two octets
    assert all(len(r) >= 5 and kermit(r[:-2]) == int.from_bytes(r[-2:], "little") for r in recs), "a record whose FCS is wrong"
elif tag == "zwave":              # the data rate (0 to 2) and the MPDU, which starts with the home ID
    assert all(r[0] in (0, 1, 2) and r[1:5] == bytes.fromhex("c0ffee01") for r in recs), "not a rate and an MPDU"
elif tag == "wifi":               # a radiotap header (version 0, its length, the flags present) and the 802.11 frame
    for r in recs:
        hl, present = struct.unpack("<HI", r[2:8])
        assert r[0] == 0 and 8 <= hl < len(r) and present & 2, "not a radiotap header"
elif tag == "ble":   # BLUETOOTH_LE_LL: the advertising access address 0x8E89BED6, little endian, then the PDU
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

# --- the four modes whose lines come from their Wireshark hub packets (zigbee, thread, zwave, wifi): OnAir's generator scene in,
# the lines its own parsers read out, at least as many pcap records as lines, and the user CPU per second of input.
# frames <tag> <rate> <freq> [onair-rx args...]: onair-gen's scene for the mode through onair-rx; the user CPU is printed
frames() {
  local tag=$1 rate=$2 hz=$3 secs
  shift 3
  gen "$tag" --mode "$tag" --rate "$rate" --freq "$hz"
  /usr/bin/time -f %U -o "$d/$tag.cpu" "$bin" --format cf32 --mode "$tag" --rate "$rate" --freq "$hz" "$@" \
    <"$d/$tag.iq" >"$d/$tag.jsonl" 2>"$d/$tag.err" || fail "$tag: onair-rx exited $?: $(tail -n 3 "$d/$tag.err")"
  secs=$(python3 -I -c 'import sys; print(f"{int(sys.argv[1]) / 8 / float(sys.argv[2]):.2f}")' "$(stat -c %s "$d/$tag.iq")" "$rate")
  echo "cpu: $tag $(cat "$d/$tag.cpu") s user over $secs s of input at $rate Hz"
}
# check <tag> <rate> <freq>: what the scene's own values say the lines must hold
check() {
  python3 -I - "$d/$1.jsonl" "$d/$1.gen" "$2" "$(samples "$1")" "$1" "$3" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp, tag, hz = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), sys.argv[5], float(sys.argv[6])
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "the last line is not ended"
ls = [json.loads(l) for l in raw.splitlines()]
assert ls, "no line"
g = re.search(r"from ([0-9.]+) s to ([0-9.]+) s", open(genlog).read())
t0, t1 = float(g.group(1)), float(g.group(2))
last = 0
hex_re = lambda n: re.compile(rf"[0-9a-f]{{{n}}}")
for n, o in enumerate(ls):
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp and t >= last, f"line {n}: t_sample {t!r} (of {nsamp}, after {last})"
    last = t
    assert t0 - 0.5 <= t / rate <= t1 + 0.5, f"line {n}: at {t / rate:.3f} s, the message is at {t0:.3f} s to {t1:.3f} s"
    assert o.get("event") == f"{tag}.frame", f"line {n}: event {o.get('event')!r}"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert type(o.get("len")) is int and o["len"] > 0, f"line {n}: len {o.get('len')!r}"
    assert type(o.get("secured")) is bool or tag == "wifi" and "secured" not in o, f"line {n}: secured {o.get('secured')!r}"
    assert not any(type(v) is float for v in o.values()), f"line {n}: a decimal number {o!r}"
by = lambda f: [o for o in ls if f(o)]
if tag in ("zigbee", "thread"):
    addr = lambda a: isinstance(a, str) and (hex_re(4).fullmatch(a) or hex_re(16).fullmatch(a))
    kinds = {"beacon", "data", "ack", "command"}
    for n, o in enumerate(ls):
        assert o.get("frame_type") in kinds, f"line {n}: frame_type {o.get('frame_type')!r}"
        assert type(o.get("seq")) is int and 0 <= o["seq"] <= 255, f"line {n}: seq {o.get('seq')!r}"
        assert "pan" not in o or hex_re(4).fullmatch(o["pan"]), f"line {n}: pan {o.get('pan')!r}"
        assert all(k not in o or addr(o[k]) for k in ("src", "dst")), f"line {n}: addresses {o!r}"
        assert all(k not in o or hex_re(4).fullmatch(o[k]) for k in ("nwk_src", "nwk_dst")), f"line {n}: nwk {o!r}"
    if tag == "zigbee":
        # OnAir's scene (zigbee_gen.cpp, zigbeeTestSchedule): PAN 1A62, coordinator 0000, a Hue bulb 1234 (extended address
        # 001788010A0B0D0C...), a beacon request, the coordinator's beacon, an association, an ack, a Device_annce in the clear,
        # a link status, encrypted NWK payloads
        assert {o["frame_type"] for o in ls} == kinds, f"frame types {sorted({o['frame_type'] for o in ls})}"
        assert by(lambda o: o["frame_type"] == "beacon" and o.get("pan") == "1a62" and o.get("src") == "0000"), "the coordinator's beacon"
        assert by(lambda o: o["frame_type"] == "command" and o.get("pan") == "ffff" and o.get("dst") == "ffff" and "src" not in o), "the beacon request"
        assert by(lambda o: o["frame_type"] == "command" and o.get("src") == "001788010a0b0c0d" and o.get("dst") == "0000"), "the association request"
        assert by(lambda o: o["frame_type"] == "command" and o.get("src") == "5a5a" and o.get("dst") == "0000"), "the data request"
        assert by(lambda o: o["frame_type"] == "ack" and "src" not in o and "dst" not in o), "the ack"
        assert by(lambda o: o.get("nwk_src") == "1234" and o.get("nwk_dst") == "fffd" and o["secured"] is False), "the Device_annce"
        assert by(lambda o: o["secured"] and o.get("nwk_src") in ("7a12", "1234", "0000")), "an encrypted NWK frame"
    else:
        # the frames of OnAir's Thread test: a beacon of PAN 7A31 from 5000, six encrypted data frames from 5400 or 5401 to 5000,
        # an MLE Discovery Request in the clear from the extended address 0123456789ABCDEF to the broadcast
        assert all(isinstance(o.get("kind"), str) and o["kind"] for o in ls), "a line without kind"
        assert not any(k in o for o in ls for k in ("nwk_src", "nwk_dst")), "Thread has no Zigbee network header"
        assert by(lambda o: o["frame_type"] == "beacon" and o.get("pan") == "7a31" and o.get("src") == "5000"), "the Thread beacon"
        data = by(lambda o: o["frame_type"] == "data" and o.get("pan") == "7a31" and o.get("dst") == "5000" and o["secured"])
        assert {o.get("src") for o in data} == {"5400", "5401"} and len(data) == 6, f"the encrypted data: {data!r}"
        mle = by(lambda o: o.get("src") == "0123456789abcdef")
        assert len(mle) == 1 and mle[0]["dst"] == "ffff" and mle[0]["pan"] == "ffff" and not mle[0]["secured"], f"the MLE request: {mle!r}"
        assert "MLE" in mle[0]["kind"], f"kind {mle[0]['kind']!r}"
elif tag == "zwave":
    # OnAir's EU scene (zwave_gen.cpp, zwaveTestSchedule): home ID C0FFEE01, controller 1, nodes 2 to 6; R1 on 868.42 MHz,
    # R2 on 868.40 MHz, R3 on 869.85 MHz; singlecast, acks, a multicast, an S0 and an S2 frame
    chan = {9600: 868420000, 40000: 868400000, 100000: 869850000}
    for n, o in enumerate(ls):
        assert o.get("home_id") == "c0ffee01", f"line {n}: home_id {o.get('home_id')!r}"
        assert type(o.get("src")) is int and 1 <= o["src"] <= 6, f"line {n}: src {o.get('src')!r}"
        assert "dst" not in o or type(o["dst"]) is int and 1 <= o["dst"] <= 255, f"line {n}: dst {o.get('dst')!r}"
        assert o.get("rate_bps") in chan, f"line {n}: rate_bps {o.get('rate_bps')!r}"
        assert o.get("freq_hz") == chan[o["rate_bps"]], f"line {n}: freq_hz {o.get('freq_hz')!r}"
        assert o.get("frame_type") in ("singlecast", "multicast", "ack", "routed"), f"line {n}: frame_type {o.get('frame_type')!r}"
    assert {o["rate_bps"] for o in ls} == set(chan), f"rates {sorted({o['rate_bps'] for o in ls})}"
    assert {"singlecast", "ack", "multicast"} <= {o["frame_type"] for o in ls}, "frame types"
    assert by(lambda o: o["frame_type"] == "multicast" and "dst" not in o and o["src"] == 1), "the multicast"
    assert by(lambda o: o["src"] == 6 and o.get("dst") == 255), "the broadcast of node 6"
    assert len(by(lambda o: o["secured"])) == 2, "the S0 and the S2 frame are the two secured frames"
else:
    # OnAir's 2.4 GHz scene (wifi_gen.cpp, wifiTestSchedule): "OnAir-Home" (WPA2, AVM 00:04:0E:12:34:56, a beacon and a probe
    # response), "OnAir-Cafe" (WPA2/WPA3, FC:EC:DA:AB:CD:EF) and a hidden open AP 02:11:22:33:44:55, on channel 6; a data frame
    # between them is a record and no line
    for n, o in enumerate(ls):
        assert o.get("frame_type") in ("beacon", "probe_response", "fils_discovery"), f"line {n}: frame_type {o.get('frame_type')!r}"
        assert hex_re(12).fullmatch(o.get("bssid", "")), f"line {n}: bssid {o.get('bssid')!r}"
        assert o.get("channel") == 6 and o.get("freq_hz") == 2437000000, f"line {n}: channel/freq_hz {o!r}"
        assert isinstance(o.get("security"), str) and o["security"], f"line {n}: security {o.get('security')!r}"
        assert "ssid" not in o or isinstance(o["ssid"], str) and o["ssid"], f"line {n}: ssid {o.get('ssid')!r}"
    home = by(lambda o: o["bssid"] == "00040e123456")
    assert home and all(o.get("ssid") == "OnAir-Home" and o["security"] == "WPA2" for o in home), f"OnAir-Home: {home!r}"
    assert {o["frame_type"] for o in home} == {"beacon", "probe_response"}, "OnAir-Home's beacon and probe response"
    cafe = by(lambda o: o["bssid"] == "fcecdaabcdef")
    assert cafe and all(o.get("ssid") == "OnAir-Cafe" and o["security"] == "WPA2/WPA3" for o in cafe), f"OnAir-Cafe: {cafe!r}"
    hidden = by(lambda o: o["bssid"] == "021122334455")
    assert hidden and all("ssid" not in o and o["security"] == "Open" for o in hidden), f"the hidden AP: {hidden!r}"
    assert {o["bssid"] for o in ls} == {"00040e123456", "fcecdaabcdef", "021122334455"}, "the scene's three access points"
print(f"ok: {len(ls)} {tag}.frame line(s)")
EOF
}
frames zigbee 4000000 "$zb_hz"
check zigbee 4000000 "$zb_hz" || fail zigbee
frames thread 4000000 "$zb_hz"
check thread 4000000 "$zb_hz" || fail thread
frames zwave 2000000 "$zw_hz"
check zwave 2000000 "$zw_hz" || fail zwave
frames wifi 20000000 "$wf_hz"
check wifi 20000000 "$wf_hz" || fail wifi
# --region EU is the region found; the same lines
"$bin" --mode zwave --rate 2000000 --freq "$zw_hz" --region EU <"$d/zwave.iq" >"$d/zwave-eu.jsonl" 2>/dev/null || fail "zwave with --region EU"
cmp -s "$d/zwave.jsonl" "$d/zwave-eu.jsonl" || fail "zwave --region EU gives other lines than the region found"
echo "ok: --region EU gives the lines of the region found from --freq"

pcap zigbee 195 --mode zigbee --rate 4000000 --freq "$zb_hz" || fail "zigbee on the pcap port"
pcap thread 195 --mode thread --rate 4000000 --freq "$zb_hz" || fail "thread on the pcap port"
pcap zwave 148 --mode zwave --rate 2000000 --freq "$zw_hz" || fail "zwave on the pcap port"
pcap wifi 127 --mode wifi --rate 20000000 --freq "$wf_hz" || fail "wifi on the pcap port"

# --- noise alone: 8 s (Wi-Fi: 2 s) of /dev/urandom as cs16 gives no line and exit 0
for row in "zigbee 4000000 $zb_hz 8" "thread 4000000 $zb_hz 8" "zwave 2000000 $zw_hz 8" "wifi 20000000 $wf_hz 2"; do
  read -r m rate hz secs <<<"$row"
  head -c $((rate * secs * 4)) /dev/urandom >"$d/noise.cs16"
  /usr/bin/time -f %U -o "$d/noise.cpu" "$bin" --mode "$m" --rate "$rate" --freq "$hz" --format cs16 <"$d/noise.cs16" \
    >"$d/noise.jsonl" 2>"$d/noise.err" || fail "$m on noise: onair-rx exited $?: $(tail -n 3 "$d/noise.err")"
  [ ! -s "$d/noise.jsonl" ] || fail "$m: a line on noise: $(head -n 1 "$d/noise.jsonl")"
  echo "ok: $m on $secs s of noise: no line, exit 0 ($(cat "$d/noise.cpu") s user over $secs s of input)"
done
rm -f "$d/noise.cs16"

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
