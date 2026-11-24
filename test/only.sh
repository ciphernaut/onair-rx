#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the modes only OnAir decodes (ble, lorawan, zigbee, thread, zwave, wifi, wisun, tetra, gsmr, lte and dvbs) on OnAir's own
# test signals: _build/onair-gen makes the scene of OnAir's generator for the mode (makeBleSynth, makeLorawanSynth,
# makeZigbeeSynth, the frames of OnAir's Thread test, makeZwaveSynth, makeWifiSynth, makeWisunSynth) with a second of noise either
# side (Wi-Fi: a tenth of a second, at 20 Msps), onair-rx decodes it. Wi-SUN, whose frames come out of a decoder thread, is also
# checked for the frames the end of input holds (the flush), runs that give the same bytes, on one CPU or two, and input that
# ends in the middle of a frame. The four modes whose lines come from their Wireshark hub packets (zigbee, thread,
# zwave, wifi) are also checked against what OnAir's generator put in the scene, their pcap link types, --freq missing, and
# noise alone (8 s of /dev/urandom: no line, exit 0); the script prints each mode's user CPU over its seconds of input. The signals are synthesised; no capture is used. Also: the options a mode does not take (--width, --plan,
# --region, --drop) are refused by every mode that does not take them, lorawan without --freq and at a --freq no channel plan covers, the frames on the
# pcap port, and onair-gen's --sigmf pair read back. TETRA and GSM-R (polled cells, decoded a stretch at a time on the
# receiver's worker; makeTetraSynth and makeGsmrSynth, scene 0, three stretches long, the generator's own noise): one line per
# cell with the scene's identity and no second one, the same bytes from two runs, on one CPU and through a pipe, --drop runs
# and ends, 1.5 s of input (under one stretch, padded at its end) gives the cells, a writer at real time is never held up
# 0.2 s, input of exactly three stretches skips nothing, their GSMTAP packets, --freq missing, a rate too low, and noise alone.
# LTE (a polled cell mode as well, decoded 0.2 s at a time; makeCellSynth's 5 MHz cell at 4.6 Msps, the lowest rate whose
# input holds the cell) the same way, with 0.19 s of input for the short cut, and --drop at 20 MHz from a writer at real time
# in writes of 4 MB (the cell read whole). DVB-S (a polled status mode; OnAir's transmitter
# chain carrying one known UDP datagram in GSE, in MPE and in ULE): a status line at first lock or 2 s, then on a change and at
# least every 2 s, the datagrams counted and on the pcap port byte for byte, --width bounding the carrier the receiver takes,
# the same bytes from runs on one CPU, through a pipe and from a writer at real time, a cut input, --drop, a --width wider
# than the rate refused, and noise alone.
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
ws_hz=921500000      # the centre of OnAir's Wi-SUN scene (wisunTuning().defMhz), between two ANZ channels
"$bin" --mode wisun --rate 2000000 --freq "$ws_hz" </dev/null >"$d/wisun.empty" 2>"$d/wisun.empty.err" ||
  fail "wisun: $(cat "$d/wisun.empty.err")"
[ ! -s "$d/wisun.empty" ] || fail "wisun: a line on empty input"
echo "ok: onair-rx has wisun"
te_hz=392500000      # the centre of OnAir's TETRA scene (kTetraTestCentreHz); its cell at 392.4125 MHz
gr_hz=923000000      # the middle of OnAir's GSM-R scene 0 (ARFCN 960 at 922.2 MHz, 968 at 923.8 MHz): 2 MHz holds both
for row in "tetra $te_hz" "gsmr $gr_hz"; do
  read -r m hz <<<"$row"
  "$bin" --mode "$m" --rate 2000000 --freq "$hz" </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" || fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has tetra and gsmr"
lte_hz=1815000000    # the 5 MHz cell of OnAir's LTE scene 1 (cell_gen.cpp: band 3, EARFCN 1300)
lte_rate=4600000     # the lowest rate of 100 kHz steps whose input holds that cell's 4.5 MHz (4.4 Msps reads its MIB, not SIB1)
"$bin" --mode lte --rate "$lte_rate" --freq "$lte_hz" </dev/null >"$d/lte.empty" 2>"$d/lte.empty.err" ||
  fail "lte: $(cat "$d/lte.empty.err")"
[ ! -s "$d/lte.empty" ] || fail "lte: a line on empty input"
"$bin" --mode dvbs --rate 10000000 --width 8000000 </dev/null >"$d/dvbs.empty" 2>"$d/dvbs.empty.err" ||
  fail "dvbs: $(cat "$d/dvbs.empty.err")"
[ ! -s "$d/dvbs.empty" ] || fail "dvbs: a line on empty input"
echo "ok: onair-rx has lte and dvbs"

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
refuses "onair-rx: wisun needs --freq HZ" --mode wisun --rate 2000000 || fail "wisun without --freq"
refuses "onair-rx: 1000000 Hz is too low for wisun (at least 2000000 Hz)" --mode wisun --rate 1000000 --freq "$ws_hz" ||
  fail "wisun at 1 MHz"
refuses "onair-rx: wisun: no region XX (ANZ, NA, MX, BZ, JP, KR, PH, MY, HK, SG, SG866, TH, VN, EU863, EU870, IN, CN470, CN779, CN920, WW)" \
  --mode wisun --rate 2000000 --freq "$ws_hz" --region XX || fail "wisun with an unknown --region"
refuses "onair-rx: wisun: region EU863 has no channel within 920.600-922.400 MHz" --mode wisun --rate 2000000 --freq "$ws_hz" \
  --region EU863 || fail "wisun with a --region that has no channel in the input"
refuses "onair-rx: wisun: region ANZ (OnAir's pick for 100.000 MHz) has no channel within 99.100-100.900 MHz" --mode wisun \
  --rate 2000000 --freq 100000000 || fail "wisun at a --freq whose region has no channel in the input"
refuses "onair-rx: tetra needs --freq HZ" --mode tetra --rate 2000000 || fail "tetra without --freq"
refuses "onair-rx: gsmr needs --freq HZ" --mode gsmr --rate 2000000 || fail "gsmr without --freq"
refuses "onair-rx: 100000 Hz is too low for tetra (at least 200000 Hz)" --mode tetra --rate 100000 --freq "$te_hz" ||
  fail "tetra at 100 kHz"
refuses "onair-rx: 500000 Hz is too low for gsmr (at least 1000000 Hz)" --mode gsmr --rate 500000 --freq "$gr_hz" ||
  fail "gsmr at 500 kHz"
refuses "onair-rx: lte needs --freq HZ" --mode lte --rate "$lte_rate" || fail "lte without --freq"
refuses "onair-rx: 1000000 Hz is too low for lte (at least 1920000 Hz)" --mode lte --rate 1000000 --freq "$lte_hz" ||
  fail "lte at 1 MHz"
refuses "onair-rx: 1000000 Hz is too low for dvbs (at least 2000000 Hz)" --mode dvbs --rate 1000000 || fail "dvbs at 1 MHz"
refuses "onair-rx: dvbs: --width 12000000 is wider than --rate 10000000" --mode dvbs --rate 10000000 --width 12000000 ||
  fail "dvbs with a --width wider than its --rate"

# --- the options a mode does not take: every mode before this plan refuses all four; ble, zigbee, thread and wifi all four;
# lorawan all but --plan; zwave all but --region; wisun --width and --plan; tetra, gsmr and lte --width, --plan and
# --region; dvbs --plan and --region. Each mode gets the rate and the --freq it needs, so that the option is the only thing wrong
for row in "adsb 2400000" "pocsag 1000000" "flex 1000000" "acars 1000000 131525000" "aprs 240000" "ais 250000 162000000" \
  "vdl2 250000 136975000" "hfdl 24000 8927000" "rs41 2000000" "dfm 2000000" "m20 2000000" "rds 500000" "dmr 48000" \
  "p25 250000" "m17 250000" "dvbt 10000000" "video 8000000" "ble 4000000" "lorawan 2000000 $lora_hz" \
  "zigbee 4000000 $zb_hz" "thread 4000000 $zb_hz" "zwave 2000000 $zw_hz" "wifi 20000000 $wf_hz" "wisun 2000000 $ws_hz" \
  "tetra 2000000 $te_hz" "gsmr 2000000 $gr_hz" "lte $lte_rate $lte_hz" "dvbs 10000000"; do
  read -r m rate hz <<<"$row"
  f=(--mode "$m" --rate "$rate")
  [ -z "${hz:-}" ] || f+=(--freq "$hz")
  for opt in "width 2000000" "plan EU868" "region EU" "drop"; do
    read -r o v <<<"$opt"
    case "$m/$o" in lorawan/plan | zwave/region | wisun/region | wisun/drop | tetra/drop | gsmr/drop | lte/drop | dvbs/width | \
      dvbs/drop) continue ;; esac
    a=(--"$o")
    [ -z "${v:-}" ] || a+=("$v")
    refuses "onair-rx: $m takes no --$o" "${f[@]}" "${a[@]}" >/dev/null || fail "$m with --$o"
  done
done
echo "ok: every mode refuses the options it does not take (lorawan takes --plan, zwave --region, wisun --region and --drop," \
  "tetra, gsmr and lte --drop, dvbs --width and --drop)"

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
import json, re, socket, struct, subprocess, sys, threading, time
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
elif tag == "wisun":              # the MPDU without its FCS (link type 230), one per line: the line's len is the record's
    lens = [json.loads(l)["len"] for l in out.splitlines()]
    assert [len(r) for r in recs] == lens, f"records of {[len(r) for r in recs]} octets for lines of len {lens}"
elif tag in ("tetra", "gsmr", "lte"):   # GSMTAP in IPv4 + UDP to port 4729, as OnAir's capture files have it: OnAir writes
    # version 1 with a header of 4 words (wire_format.cpp, makeGsmtap); TETRA's blocks are type 5 (TETRA I1), GSM-R's BCCH blocks
    # type 1 (Um), LTE's system information type 13 (LTE RRC)
    want_type = {"tetra": 5, "gsmr": 1, "lte": 13}[tag]
    for r in recs:
        assert r[0] >> 4 == 4 and r[9] == 17, "not IPv4 carrying UDP"
        ihl = (r[0] & 15) * 4
        dport, = struct.unpack(">H", r[ihl + 2:ihl + 4])
        assert dport == 4729, f"UDP to port {dport}, not 4729"
        g = r[ihl + 8:]
        assert g[0] == 1 and g[1] == 4 and g[2] == want_type, f"GSMTAP version {g[0]}, header {g[1]} words, type {g[2]}"
        if tag == "lte":   # BCCH-DL-SCH (sub-type 5) on the cell's EARFCN (scene 1: 1300)
            assert g[12] == 5 and struct.unpack(">H", g[4:6])[0] == 1300, f"sub-type {g[12]}, EARFCN {struct.unpack('>H', g[4:6])[0]}"
elif tag.startswith("dvbs"):   # whole IP datagrams as they are (link type RAW): each one the datagram the generator sent
    want = bytes.fromhex(re.search(r"datagram ([0-9a-f]+)", open(iq[:-3] + ".gen").read()).group(1))
    assert all(r == want for r in recs), f"a record that is not the datagram sent: {next(r for r in recs if r != want).hex()}"
    n = json.loads(out.splitlines()[-1])["datagrams"]   # the datagrams so far at the last line, which may come before the end
    assert n >= 1 and len(recs) >= n, f"{len(recs)} record(s), the last line counts {n} datagram(s)"
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

# --- Wi-SUN: OnAir's FAN scene (makeWisunSynth) at 2 MHz around 921.5 MHz, between two of its ANZ channels: the frames come out
# of the receiver's decoder thread, and those it still holds when the input ends only through its flush.
# wscheck <jsonl> <tag of the generator's run> <rate> <samples>: a line per frame of the scene, in its order, each with the
# frame's channel and bit rate and the scene's addresses; t_sample next to the frame's end. Prints how far t_sample trails it.
wscheck() {
  python3 -I - "$1" "$d/$2.gen" "$3" "$4" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "the last line is not ended"
ls = [json.loads(l) for l in raw.splitlines()]
log = open(genlog).read()
t0, t1 = map(float, re.search(r"message from ([0-9.]+) s to ([0-9.]+) s", log).groups())
frames = [(float(a), float(b), int(br), int(hz)) for a, b, br, hz in
          re.findall(r"frame \d+ from ([0-9.]+) s to ([0-9.]+) s, (\d+) bit/s on (\d+) Hz", log)]
assert frames, "the generator did not say where the frames are"
assert len(ls) == len(frames), f"{len(ls)} line(s) for the scene's {len(frames)} frames"
hex_re = lambda n: re.compile(rf"[0-9a-f]{{{n}}}")
types = {"pan_advert", "pan_advert_solicit", "pan_config", "pan_config_solicit", "data", "ack", "eapol"}
last, trail = 0, []
for n, (o, (a, b, br, hz)) in enumerate(zip(ls, frames)):
    assert o.get("event") == "wisun.frame", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp and t >= last, f"line {n}: t_sample {t!r} (of {nsamp}, after {last})"
    last = t
    assert t0 - 0.5 <= t / rate <= t1 + 0.5, f"line {n}: at {t / rate:.3f} s, the message is at {t0:.3f} s to {t1:.3f} s"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert not any(type(v) is float for v in o.values()), f"line {n}: a decimal number {o!r}"
    assert o.get("frame_type") in types, f"line {n}: frame_type {o.get('frame_type')!r}"
    assert "seq" not in o or type(o["seq"]) is int and 0 <= o["seq"] <= 255, f"line {n}: seq {o.get('seq')!r}"
    assert "pan" not in o or hex_re(4).fullmatch(o["pan"]), f"line {n}: pan {o.get('pan')!r}"
    assert all(k not in o or hex_re(16).fullmatch(o[k]) for k in ("src", "dst")), f"line {n}: addresses {o!r}"
    assert type(o.get("len")) is int and o["len"] > 0, f"line {n}: len {o.get('len')!r}"
    assert o.get("freq_hz") == hz and o.get("rate_bps") == br, f"line {n}: {o.get('freq_hz')!r} Hz, {o.get('rate_bps')!r} bit/s; the frame was on {hz} Hz at {br} bit/s"
    trail.append(t / rate - b)
by = lambda f: [o for o in ls if f(o)]
br, router, meter = "00124bfffe1a2b3c", "000d6ffffe445566", "02a1b2fffec3d4e5"
# OnAir's scene (wisun_gen.cpp, wisunTestSchedule): PAN 7A31; the border router's and the router's PAN adverts, the meter's
# solicit and its EAPOL exchange through the router, a PAN configuration solicit and two PAN configurations, three data frames
# meter > router (seq 10 to 12) and the router's acks, the border router's broadcast data (seq 0x55), a solicit at 50 kbit/s
assert {o["src"] for o in ls if "src" in o} == {br, router, meter}, f"sources {sorted({o.get('src') for o in ls})}"
assert {o["frame_type"] for o in ls} == types, f"frame types {sorted({o['frame_type'] for o in ls})}"
assert {o["src"] for o in by(lambda o: o["frame_type"] == "pan_advert" and o.get("pan") == "7a31")} == {br, router}, "the two PAN adverts"
assert by(lambda o: o["frame_type"] == "pan_advert_solicit" and o.get("src") == meter), "the meter's PAN advert solicit"
assert by(lambda o: o["frame_type"] == "eapol" and o.get("src") == meter and o.get("dst") == router), "the meter's EAPOL"
assert by(lambda o: o["frame_type"] == "eapol" and o.get("src") == router and o.get("dst") == meter), "the router's EAPOL"
data = by(lambda o: o["frame_type"] == "data" and o.get("src") == meter and o.get("dst") == router)
assert sorted(o.get("seq") for o in data) == [10, 11, 12], f"the meter's data: {data!r}"
acks = by(lambda o: o["frame_type"] == "ack" and o.get("src") == router and o.get("dst") == meter)
assert sorted(o.get("seq") for o in acks) == [10, 11, 12], f"the router's acks: {acks!r}"
assert by(lambda o: o["frame_type"] == "data" and o.get("src") == br and o.get("seq") == 0x55 and "dst" not in o), "the broadcast data"
assert {o["rate_bps"] for o in ls} == {50000, 150000}, f"bit rates {sorted({o['rate_bps'] for o in ls})}"
assert by(lambda o: o["rate_bps"] == 50000 and o["frame_type"] == "pan_config_solicit" and o.get("src") == router), "the 50 kbit/s solicit"
print(f"ok: {len(ls)} wisun.frame line(s), one per frame of the scene, on {len({o['freq_hz'] for o in ls})} channel(s); "
      f"t_sample from {min(trail) * 1e3:.1f} ms to {max(trail) * 1e3:.1f} ms after the frame's end")
# the bound the README gives: within one input block (16384 samples) and 2 ms of the frame's end
blk = 16384 / rate + 0.002
assert all(-blk <= x <= blk for x in trail), f"t_sample {min(trail) * 1e3:.1f} to {max(trail) * 1e3:.1f} ms from the frame's end"
EOF
}
frames wisun 2000000 "$ws_hz"
wscheck "$d/wisun.jsonl" wisun 2000000 "$(samples wisun)" || fail wisun
# the same input again, and on one CPU and on two: the same bytes (no line depends on when the decoder thread ran)
rx wisun --mode wisun --rate 2000000 --freq "$ws_hz"
cp "$d/wisun.jsonl" "$d/wisun-1.jsonl"
rx wisun --mode wisun --rate 2000000 --freq "$ws_hz"
cmp -s "$d/wisun-1.jsonl" "$d/wisun.jsonl" || fail "wisun: a second run gives other bytes"
for cpus in 0 0,1; do
  taskset -c "$cpus" "$bin" --format cf32 --mode wisun --rate 2000000 --freq "$ws_hz" <"$d/wisun.iq" >"$d/wisun-cpu.jsonl" \
    2>"$d/wisun-cpu.err" || fail "wisun on CPU $cpus: $(tail -n 3 "$d/wisun-cpu.err")"
  cmp -s "$d/wisun-1.jsonl" "$d/wisun-cpu.jsonl" || fail "wisun on CPU $cpus gives other bytes"
done
echo "ok: wisun: two runs, and runs on CPU 0 and on CPUs 0 and 1, give the same bytes"
# --region ANZ is the region found from --freq: the same bytes
"$bin" --mode wisun --rate 2000000 --freq "$ws_hz" --region ANZ <"$d/wisun.iq" >"$d/wisun-anz.jsonl" 2>/dev/null ||
  fail "wisun with --region ANZ"
cmp -s "$d/wisun-1.jsonl" "$d/wisun-anz.jsonl" || fail "wisun --region ANZ gives other lines than the region found"
echo "ok: --region ANZ gives the lines of the region found from --freq"
# cutiq <tag> <seconds>: the first seconds of $d/wisun.iq into $d/<tag>.iq
cutiq() { head -c "$(python3 -I -c 'import sys; print(round(float(sys.argv[1]) * 2000000) * 8)' "$2")" "$d/wisun.iq" >"$d/$1.iq"; }
ws_end=$(sed -n 's/.*message from [0-9.]* s to \([0-9.]*\) s.*/\1/p' "$d/wisun.gen")
# the input cut 0.2 s after the message's end: the last frame still comes (the flush), the same bytes
cutiq wisun-end "$(python3 -I -c 'import sys; print(float(sys.argv[1]) + 0.2)' "$ws_end")"
rx wisun-end --mode wisun --rate 2000000 --freq "$ws_hz"
cmp -s "$d/wisun-1.jsonl" "$d/wisun-end.jsonl" || fail "wisun: the input cut 0.2 s after the message gives other lines"
echo "ok: wisun: the input cut 0.2 s after the message's end gives every line"
# with --drop the decoder thread is not waited for while the input comes: the last frame, 1 ms before the input ends, comes only
# from the flush; the same frames (t_sample, freq_hz and rate_bps aside: frames handed over between two of the receiver's
# reports are not in its report yet)
ws_last_end=$(sed -n 's/.*frame [0-9]* from [0-9.]* s to \([0-9.]*\) s.*/\1/p' "$d/wisun.gen" | tail -n 1)
cutiq wisun-drop "$(python3 -I -c 'import sys; print(float(sys.argv[1]) + 0.001)' "$ws_last_end")"
rx wisun-drop --mode wisun --rate 2000000 --freq "$ws_hz" --drop
python3 -I -c '
import json, sys
strip = lambda p: [{k: v for k, v in json.loads(l).items() if k not in ("t_sample", "freq_hz", "rate_bps")} for l in open(p, "rb")]
a, b = strip(sys.argv[1]), strip(sys.argv[2])
assert a == b, f"{len(b)} line(s) with --drop, {len(a)} without; or other members"
print(f"ok: wisun --drop, the input cut 1 ms after the last frame: the {len(b)} frames, the last from the flush")' \
  "$d/wisun-1.jsonl" "$d/wisun-drop.jsonl" || fail "wisun with --drop"
# the input cut in the middle of a frame (the middle one of the scene): exit 0 within 5 s, the lines of the frames before it
ws_mid=$(python3 -I -c '
import re, sys
f = re.findall(r"frame \d+ from ([0-9.]+) s to ([0-9.]+) s", open(sys.argv[1]).read())
a, b = map(float, f[len(f) // 2])
print((a + b) / 2)' "$d/wisun.gen")
cutiq wisun-mid "$ws_mid"
st=0
timeout 5 "$bin" --format cf32 --mode wisun --rate 2000000 --freq "$ws_hz" <"$d/wisun-mid.iq" >"$d/wisun-mid.jsonl" \
  2>"$d/wisun-mid.err" || st=$?
[ "$st" = 0 ] || fail "wisun on input cut in the middle of a frame: exit $st (124: not within 5 s): $(tail -n 3 "$d/wisun-mid.err")"
python3 -I -c '
import sys
a, b = open(sys.argv[1], "rb").read().splitlines(), open(sys.argv[2], "rb").read().splitlines()
assert b == a[:len(a) // 2], f"{len(b)} line(s); want the first {len(a) // 2} of {len(a)}"
print(f"ok: wisun on input cut in the middle of frame {len(a) // 2 + 1}: exit 0 within 5 s, the {len(b)} lines before it")' \
  "$d/wisun-1.jsonl" "$d/wisun-mid.jsonl" || fail "wisun on input cut in the middle of a frame"
pcap wisun 230 --mode wisun --rate 2000000 --freq "$ws_hz" || fail "wisun on the pcap port"

# --- TETRA and GSM-R: polled cells, decoded a stretch of input at a time on the receiver's own worker (TETRA 2.3 s, GSM-R
# 2.2 s). onair-gen plays OnAir's scene 0 (makeTetraSynth: one cell; makeGsmrSynth: two) for three stretches and a little,
# with the generator's own noise, and says each cell's identity: "cell N at F Hz: ...".
# cellcheck <tag> <rate> <stretch s>: one line per generated cell with the scene's identity, at the first poll after the first
# stretch ends, and no second line (the scene's cells do not change); no null, and no decimal number but snr_db
cellcheck() {
  python3 -I - "$d/$1.jsonl" "$d/$1.gen" "$2" "$(samples "$1")" "$1" "$3" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp, tag, stretch = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), sys.argv[5], float(sys.argv[6])
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "the last line is not ended"
ls = [json.loads(l) for l in raw.splitlines()]
log = open(genlog).read()
want = []
for hz, rest in re.findall(r"cell \d+ at (\d+) Hz: (.*)", log):
    w = {"freq_hz": int(hz)}
    sibs = re.search(r"sibs ([0-9 ]+)", rest)   # lte: the SIB numbers the cell sends, ascending
    if sibs:
        w["sibs"] = [int(x) for x in sibs.group(1).split()]
        rest = rest[:sibs.start()]
    for k, v in re.findall(r"([a-z_]+) ([0-9]+)", rest):
        w[k] = int(v)
    w["network_time"] = "network time" in rest
    want.append(w)
assert want, "the generator did not say its cells"
assert len(ls) == len(want), f"{len(ls)} line(s) for the scene's {len(want)} cell(s): {ls!r}"
first, last = int(stretch * rate), 0
for n, o in enumerate(ls):
    assert o.get("event") == f"{tag}.cell", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp and t >= last, f"line {n}: t_sample {t!r} (of {nsamp}, after {last})"
    last = t
    # the first poll after the first stretch ends: the block that completes it, or the poll interval's (0.25 s) end after it
    assert first - 16384 < t <= first + 0.25 * rate, f"line {n}: at {t / rate:.3f} s, the first stretch ends at {stretch} s"
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert all(type(v) in (int, str) for k, v in o.items() if k not in ("snr_db", "sibs")), f"line {n}: a decimal number {o!r}"
    assert "sibs" not in o or (o["sibs"] and all(type(x) is int for x in o["sibs"]) and o["sibs"] == sorted(set(o["sibs"]))), \
        f"line {n}: sibs {o.get('sibs')!r}"
    assert type(o.get("snr_db")) in (int, float) and 10 < o["snr_db"] < 40, f"line {n}: snr_db {o.get('snr_db')!r}"
got = sorted(ls, key=lambda o: o["freq_hz"])
for o, w in zip(got, sorted(want, key=lambda w: w["freq_hz"])):
    for k, v in w.items():
        if k == "network_time":
            assert (isinstance(o.get(k), str) and o[k] != "") == v, f"network_time {o.get(k)!r}, the scene has {'one' if v else 'none'}"
        else:
            assert o.get(k) == v, f"{k} {o.get(k)!r}, the scene's cell at {w['freq_hz']} Hz has {v}: {o!r}"
print(f"ok: {len(ls)} {tag}.cell line(s), one per cell of the scene: " + "; ".join(
    ", ".join(f"{k} {v}" for k, v in o.items() if k not in ("event", "t_sample")) for o in got))
EOF
}
# cellrun <tag> <freq> <stretch s> [<rate> <cut samples>]: the scene at the rate (default 2 MHz), its lines checked; two runs, a
# run on CPU 0 and one through a pipe give the same bytes; the user CPU and the wall time per second of input, with and without
# --drop; the input cut to its first <cut samples> (default 3000000, 1.5 s at 2 MHz), shorter than a stretch
cellrun() {
  local m=$1 hz=$2 stretch=$3 rate=${4:-2000000} cut=${5:-3000000} secs cutsecs
  gen "$m" --mode "$m" --rate "$rate" --freq "$hz"
  /usr/bin/time -f "%U %e" -o "$d/$m.cpu" "$bin" --format cf32 --mode "$m" --rate "$rate" --freq "$hz" \
    <"$d/$m.iq" >"$d/$m.jsonl" 2>"$d/$m.err" || fail "$m: onair-rx exited $?: $(tail -n 3 "$d/$m.err")"
  secs=$(python3 -I -c 'import sys; print(f"{int(sys.argv[1]) / 8 / float(sys.argv[2]):.2f}")' "$(stat -c %s "$d/$m.iq")" "$rate")
  cutsecs=$(python3 -I -c 'import sys; print(f"{int(sys.argv[1]) / float(sys.argv[2]):g}")' "$cut" "$rate")
  read -r user wall <"$d/$m.cpu"
  echo "cpu: $m $user s user, $wall s wall over $secs s of input at $rate Hz"
  cellcheck "$m" "$rate" "$stretch" || fail "$m"
  cp "$d/$m.jsonl" "$d/$m-1.jsonl"
  rx "$m" --mode "$m" --rate "$rate" --freq "$hz"
  cmp -s "$d/$m-1.jsonl" "$d/$m.jsonl" || fail "$m: a second run gives other bytes"
  taskset -c 0 "$bin" --format cf32 --mode "$m" --rate "$rate" --freq "$hz" <"$d/$m.iq" >"$d/$m-cpu.jsonl" 2>"$d/$m-cpu.err" ||
    fail "$m on CPU 0: $(tail -n 3 "$d/$m-cpu.err")"
  cmp -s "$d/$m-1.jsonl" "$d/$m-cpu.jsonl" || fail "$m on CPU 0 gives other bytes"
  cat "$d/$m.iq" | "$bin" --format cf32 --mode "$m" --rate "$rate" --freq "$hz" >"$d/$m-pipe.jsonl" 2>"$d/$m-pipe.err" ||
    fail "$m through a pipe: $(tail -n 3 "$d/$m-pipe.err")"
  cmp -s "$d/$m-1.jsonl" "$d/$m-pipe.jsonl" || fail "$m through a pipe gives other bytes"
  echo "ok: $m: two runs, a run on CPU 0 and one through a pipe give the same bytes"
  # nothing skipped: the input cut to exactly three stretches gives the packets and the lines of the same input with 0.05 s of
  # zeros after it. That is three whole stretches and then a last partial one of zeros, which the end of input pads and
  # decodes (no carrier, no packet). A receiver that skipped input at a stretch's end would hold the third stretch
  # incomplete at the end of the exact cut, whose end then pads nothing, while the zeros complete it. The packets are counted
  # with a pcap port no client connects to (stderr's "<n> packets")
  python3 -I - "$bin" "$d/$m.iq" "$m" "$hz" "$stretch" "$rate" <<'EOF' || fail "$m on exactly three stretches"
import re, socket, subprocess, sys
binary, iq, m, hz, stretch, rate = sys.argv[1:7]
n = int(float(rate) * float(stretch)) * 3   # the receiver's own expression of a stretch (configure()), three times
data = open(iq, "rb").read()
assert len(data) // 8 > n, "the input is not longer than three stretches"
def run(raw):
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    p = subprocess.run([binary, "--format", "cf32", "--mode", m, "--rate", rate, "--freq", hz, "--pcap-port", str(port)],
                       input=raw, capture_output=True, timeout=120)
    assert p.returncode == 0, f"exit {p.returncode}: {p.stderr.decode(errors='replace')}"
    g = re.search(rb": \d+ lines, (\d+) packets", p.stderr)
    assert g, f"no packet count: {p.stderr.decode(errors='replace')}"
    return int(g.group(1)), p.stdout
cut, cl = run(data[:n * 8])
zeros, zl = run(data[:n * 8] + bytes(int(0.05 * float(rate)) * 8))
assert cut > 0 and cut == zeros, f"{cut} packets from exactly three stretches, {zeros} with 0.05 s of zeros after them"
assert cl == zl, "the lines differ"
print(f"ok: {m}: the input cut to exactly three stretches ({n} samples): the {cut} packets and the lines of it with zeros after")
EOF
  # the reading of stdin never waits for the receiver: a writer at real time in blocks of 16384 samples is never held up
  # by a write for 0.2 s or more, while every stretch is still decoded (the lines of the run from a file). A mode that decodes
  # more slowly than real time (lte: CELLRUN_SLOW=1) holds the writer up whatever its read-ahead: only its lines are checked
  python3 -I - "$bin" "$d/$m.iq" "$m" "$hz" "$d/$m-1.jsonl" "$rate" "${CELLRUN_SLOW:-0}" <<'EOF' ||
import subprocess, sys, time
binary, iq, m, hz, ref, rates, slow = sys.argv[1:8]
rate, chunk = float(rates), 16384 * 8
data = open(iq, "rb").read()
p = subprocess.Popen([binary, "--format", "cf32", "--mode", m, "--rate", rates, "--freq", hz],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
import threading
got = []
th = threading.Thread(target=lambda: got.append(p.stdout.read())); th.start()
t0, worst = time.monotonic(), 0.0
for i in range(0, len(data), chunk):
    due = t0 + (i / 8) / rate
    now = time.monotonic()
    if now < due:
        time.sleep(due - now)
    w = time.monotonic()
    p.stdin.write(data[i:i + chunk]); p.stdin.flush()
    worst = max(worst, time.monotonic() - w)
p.stdin.close()
th.join(60); p.wait(60)
assert p.returncode == 0, f"exit {p.returncode}"
assert got[0] == open(ref, "rb").read(), "the lines differ from the run from a file"
if slow == "1":
    print(f"stall: {m}: slower than real time, a writer at real time held up at most {worst * 1000:.1f} ms by a write; the lines of the run from a file")
    sys.exit(0)
print(f"stall: {m}: a writer at real time held up at most {worst * 1000:.1f} ms by a write")
assert worst < 0.2, f"a write held the writer up {worst:.3f} s"
EOF
    fail "$m: a writer at real time is held up"
  # --drop: the receiver skips the input that comes while its worker is busy, as it does live; it runs and ends (its lines
  # depend on the worker's timing and are not compared)
  /usr/bin/time -f "%U %e" -o "$d/$m.cpu" "$bin" --format cf32 --mode "$m" --rate "$rate" --freq "$hz" --drop \
    <"$d/$m.iq" >"$d/$m-drop.jsonl" 2>"$d/$m-drop.err" || fail "$m --drop: onair-rx exited $?: $(tail -n 3 "$d/$m-drop.err")"
  read -r user wall <"$d/$m.cpu"
  echo "ok: $m --drop runs and ends: $(wc -l <"$d/$m-drop.jsonl") line(s); cpu $user s user, $wall s wall over $secs s of input"
  # the input cut shorter than a stretch: the end of input pads it, so the receiver decodes it. Exit 0 within 8 s with and
  # without --drop, and without it the scene's cells, each with what the run of the whole scene said of it
  head -c $((cut * 8)) "$d/$m.iq" >"$d/$m-cut.iq"
  for extra in "" --drop; do
    st=0
    # shellcheck disable=SC2086
    timeout 8 "$bin" --format cf32 --mode "$m" --rate "$rate" --freq "$hz" $extra <"$d/$m-cut.iq" >"$d/$m-cut.jsonl" \
      2>"$d/$m-cut.err" || st=$?
    [ "$st" = 0 ] || fail "$m $extra on $cutsecs s of input: exit $st (124: not within 8 s): $(tail -n 3 "$d/$m-cut.err")"
  done
  rx "$m-cut" --mode "$m" --rate "$rate" --freq "$hz"
  python3 -I -c '
import json, sys
strip = lambda p: sorted(({k: v for k, v in json.loads(l).items() if k not in ("t_sample", "snr_db")} for l in open(p, "rb")),
                         key=lambda o: o["freq_hz"])
a, b = strip(sys.argv[1]), strip(sys.argv[2])
assert a and a == b, f"{sys.argv[4]} s gives {b!r}; the whole scene {a!r}"
print(f"ok: {sys.argv[3]} on {sys.argv[4]} s of input (shorter than a stretch), with and without --drop: exit 0 within 8 s; the {len(b)} cell line(s) of the scene")' \
    "$d/$m-1.jsonl" "$d/$m-cut.jsonl" "$m" "$cutsecs" || fail "$m on $cutsecs s of input"
}
cellrun tetra "$te_hz" 2.3
pcap tetra 101 --mode tetra --rate 2000000 --freq "$te_hz" || fail "tetra on the pcap port"
cellrun gsmr "$gr_hz" 2.2
pcap gsmr 101 --mode gsmr --rate 2000000 --freq "$gr_hz" || fail "gsmr on the pcap port"
# LTE: the 5 MHz cell of OnAir's scene 1 at the lowest rate that holds it, three of the receiver's 0.2 s blocks and 0.3 s
# more. The scene's cell sends SIB1 to SIB5 in each block, so its one line has the PCI, the MIB's bandwidth and SIB1's MCC,
# MNC, TAC and cell identity, and the SIBs read. The short cut is 0.19 s, 95 % of a block: the receiver's search tracks the
# cell over the whole block, so a block padded with more zeros loses it (0.17 s reads SIB2 no more, 0.16 s no cell; README)
CELLRUN_SLOW=1 cellrun lte "$lte_hz" 0.2 "$lte_rate" $((lte_rate * 19 / 100))
pcap lte 101 --mode lte --rate "$lte_rate" --freq "$lte_hz" || fail "lte on the pcap port"
# lte --drop at 20 MHz, the rate of ZetaSDR's entry, from a writer at real time that hands its input over in writes of 4 MB
# (cs16: 50 ms, the blocks a capture_decode replay writes): the scene looped to 7.2 s. The read-ahead holds such bursts
# (under --drop it is sized by time, at least 0.25 s), and what the receiver decodes is contiguous within each of its
# blocks, so the cell is read whole: at least one line with mcc, mnc and the SIBs. (0.4.0 as first built dropped a block of
# 16384 samples from inside nearly every 0.2 s block at this write size: no line at all.)
"$gen" --mode lte --rate 20000000 --freq "$lte_hz" --sigmf "$d/lte20" 2>"$d/lte20.gen" || fail "onair-gen --mode lte at 20 MHz: $(cat "$d/lte20.gen")"
python3 -I - "$bin" "$d/lte20.sigmf-data" "$lte_hz" <<'EOF' || fail "lte --drop at 20 MHz from a writer of 4 MB writes"
import json, re, subprocess, sys, threading, time
binary, path, hz = sys.argv[1:4]
rate, wb, secs = 20000000.0, 4 << 20, 7.2
data = open(path, "rb").read()
total = int(secs * rate) * 4
p = subprocess.Popen([binary, "--format", "cs16", "--mode", "lte", "--rate", "20000000", "--freq", hz, "--drop"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
got = {}
ths = [threading.Thread(target=lambda k=k, f=f: got.__setitem__(k, f.read())) for k, f in (("out", p.stdout), ("err", p.stderr))]
for th in ths: th.start()
t0, off = time.monotonic(), 0
while off < total:
    due = t0 + (off / 4) / rate
    now = time.monotonic()
    if now < due:
        time.sleep(due - now)
    n = min(wb, total - off)
    i = off % len(data)
    chunk = data[i:i + n]
    if len(chunk) < n:
        chunk += data[:n - len(chunk)]
    p.stdin.write(chunk); p.stdin.flush()
    off += n
p.stdin.close()
for th in ths: th.join(60)
p.wait(60)
err = got["err"].decode(errors="replace")
assert p.returncode == 0, f"exit {p.returncode}: {err}"
ls = [json.loads(l) for l in got["out"].splitlines()]
g = re.search(r"onair-rx: lte: (\d+) samples dropped", err)
whole = [o for o in ls if "mcc" in o and "mnc" in o and o.get("sibs")]
assert whole, f"no line with mcc, mnc and sibs ({g.group(1) if g else 0} samples dropped): {ls!r}"
print(f"ok: lte --drop at 20 MHz, 7.2 s at real time in writes of 4 MB: {len(ls)} line(s), the cell whole "
      f"(sibs {whole[0]['sibs']}); {g.group(1) if g else 0} samples dropped by the read-ahead")
EOF
rm -f "$d"/lte20.sigmf-*

# --- DVB-S: a polled status mode. onair-gen plays OnAir's transmitter chain for 4 s carrying one UDP datagram over and over
# (gse: DVB-S2 QPSK 2/3, the datagram in a GSE packet of every BBFRAME; mpe: DVB-S QPSK 2/3, in MPE sections; ule: DVB-S2
# QPSK 2/3 carrying a transport stream, in ULE SNDUs) and says its symbol rate and the datagram's bytes.
# dvbscheck <tag> <rate> <standard | noise> [<samples>]: every line a dvbs.status with an integer t_sample that never goes
# down, locked and datagrams (never going down) always, no null, no decimal number but snr_db; the first line at most 2 s of
# samples in and
# never more than 2 s between two lines; a locked line has the standard, the generator's symbol rate (to 1 ksym/s), the
# modcod QPSK 2/3 and snr_db, and an unlocked one none of them; some locked line, and a line with at least one datagram.
# noise: at least three lines, none locked, no datagram
dvbscheck() {
  python3 -I - "$d/$1.jsonl" "$d/$1.gen" "$2" "${4:-$(samples "$1")}" "$3" <<'EOF'
import json, re, sys
path, genlog, rate, nsamp, want = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "the last line is not ended"
ls = [json.loads(l) for l in raw.splitlines()]
last, count = 0, 0
for n, o in enumerate(ls):
    assert o.get("event") == "dvbs.status", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp and t >= last, f"line {n}: t_sample {t!r} (of {nsamp}, after {last})"
    assert t - last <= 2 * rate, f"line {n}: {(t - last) / rate:.3f} s after the line before (or the start)"
    last = t
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert all(type(v) in (bool, int, str) for k, v in o.items() if k != "snr_db"), f"line {n}: a decimal number {o!r}"
    assert type(o.get("locked")) is bool, f"line {n}: locked {o.get('locked')!r}"
    assert type(o.get("datagrams")) is int and o["datagrams"] >= count, f"line {n}: datagrams {o.get('datagrams')!r} after {count}"
    count = o["datagrams"]
    signal = ("standard", "symbol_rate", "modcod", "snr_db")
    if not o["locked"]:
        assert not any(k in o for k in signal), f"line {n}: an unlocked line with {o!r}"
        continue
    assert o.get("standard") == want, f"line {n}: standard {o.get('standard')!r}, the generator sent {want}"
    rs = int(re.search(r"at (\d+) symbol/s", open(genlog).read()).group(1))
    assert type(o.get("symbol_rate")) is int and abs(o["symbol_rate"] - rs) <= 1000, f"line {n}: symbol_rate {o.get('symbol_rate')!r}, sent {rs}"
    assert o.get("modcod") == "QPSK 2/3", f"line {n}: modcod {o.get('modcod')!r}"
    assert type(o.get("snr_db")) in (int, float), f"line {n}: snr_db {o.get('snr_db')!r}"
if want == "noise":
    assert len(ls) >= 3 and not any(o["locked"] for o in ls) and count == 0, f"{len(ls)} line(s) on noise: {ls!r}"
    print(f"ok: dvbs on {nsamp / rate:.0f} s of noise: {len(ls)} lines, none locked, no datagram, at most 2 s apart")
    sys.exit(0)
first = next((n for n, o in enumerate(ls) if o["locked"]), None)
assert first is not None, f"no locked line: {ls!r}"
assert any(o["datagrams"] >= 1 for o in ls[first:]), f"no datagram counted: {ls!r}"
print(f"ok: {sys.argv[1].split('/')[-1]}: {len(ls)} dvbs.status line(s), the first locked at {ls[first]['t_sample'] / rate:.2f} s "
      f"({', '.join(f'{k} {v}' for k, v in ls[first].items() if k not in ('event', 't_sample'))}); {count} datagram(s) by the last")
EOF
}
dv_rate=10000000
dv_width=8000000   # the job's band: the 5 Msym/s carrier's 6.75 MHz and room
gen dvbs --mode dvbs --rate "$dv_rate"
/usr/bin/time -f "%U %e" -o "$d/dvbs.cpu" "$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width "$dv_width" <"$d/dvbs.iq" \
  >"$d/dvbs.jsonl" 2>"$d/dvbs.err" || fail "dvbs: onair-rx exited $?: $(tail -n 3 "$d/dvbs.err")"
read -r user wall <"$d/dvbs.cpu"
echo "cpu: dvbs $user s user, $wall s wall over 4.00 s of input at $dv_rate Hz"
dvbscheck dvbs "$dv_rate" dvbs2 || fail "dvbs"
cp "$d/dvbs.jsonl" "$d/dvbs-1.jsonl"
rx dvbs --mode dvbs --rate "$dv_rate" --width "$dv_width"
cmp -s "$d/dvbs-1.jsonl" "$d/dvbs.jsonl" || fail "dvbs: a second run gives other bytes"
taskset -c 0 "$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width "$dv_width" <"$d/dvbs.iq" >"$d/dvbs-cpu.jsonl" \
  2>"$d/dvbs-cpu.err" || fail "dvbs on CPU 0: $(tail -n 3 "$d/dvbs-cpu.err")"
cmp -s "$d/dvbs-1.jsonl" "$d/dvbs-cpu.jsonl" || fail "dvbs on CPU 0 gives other bytes"
cat "$d/dvbs.iq" | "$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width "$dv_width" >"$d/dvbs-pipe.jsonl" \
  2>"$d/dvbs-pipe.err" || fail "dvbs through a pipe: $(tail -n 3 "$d/dvbs-pipe.err")"
cmp -s "$d/dvbs-1.jsonl" "$d/dvbs-pipe.jsonl" || fail "dvbs through a pipe gives other bytes"
echo "ok: dvbs: two runs, a run on CPU 0 and one through a pipe give the same bytes"
# a writer at real time in blocks of 16384 samples is never held up by a write for 0.2 s or more, and gets the lines of the
# run from a file
python3 -I - "$bin" "$d/dvbs.iq" "$dv_rate" "$dv_width" "$d/dvbs-1.jsonl" <<'EOF' || fail "dvbs: a writer at real time is held up"
import subprocess, sys, threading, time
binary, iq, rates, width, ref = sys.argv[1:6]
rate, chunk = float(rates), 16384 * 8
data = open(iq, "rb").read()
p = subprocess.Popen([binary, "--format", "cf32", "--mode", "dvbs", "--rate", rates, "--width", width],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
got = []
th = threading.Thread(target=lambda: got.append(p.stdout.read())); th.start()
t0, worst = time.monotonic(), 0.0
for i in range(0, len(data), chunk):
    due = t0 + (i / 8) / rate
    now = time.monotonic()
    if now < due:
        time.sleep(due - now)
    w = time.monotonic()
    p.stdin.write(data[i:i + chunk]); p.stdin.flush()
    worst = max(worst, time.monotonic() - w)
p.stdin.close()
th.join(60); p.wait(60)
assert p.returncode == 0, f"exit {p.returncode}"
assert got[0] == open(ref, "rb").read(), "the lines differ from the run from a file"
print(f"stall: dvbs: a writer at real time held up at most {worst * 1000:.1f} ms by a write")
assert worst < 0.2, f"a write held the writer up {worst:.3f} s"
EOF
# --drop: the receiver drops what its decoder threads have no time for, as it does live; it runs and ends
/usr/bin/time -f "%U %e" -o "$d/dvbs.cpu" "$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width "$dv_width" --drop \
  <"$d/dvbs.iq" >"$d/dvbs-drop.jsonl" 2>"$d/dvbs-drop.err" || fail "dvbs --drop: onair-rx exited $?: $(tail -n 3 "$d/dvbs-drop.err")"
read -r user wall <"$d/dvbs.cpu"
echo "ok: dvbs --drop runs and ends: $(wc -l <"$d/dvbs-drop.jsonl") line(s); cpu $user s user, $wall s wall over 4.00 s of input"
# the input cut to 1 s: exit 0 within 8 s, a locked line and the datagrams of that second
head -c $((dv_rate * 8)) "$d/dvbs.iq" >"$d/dvbs-cut.iq"
cp "$d/dvbs.gen" "$d/dvbs-cut.gen"
st=0
timeout 8 "$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width "$dv_width" <"$d/dvbs-cut.iq" >"$d/dvbs-cut.jsonl" \
  2>"$d/dvbs-cut.err" || st=$?
[ "$st" = 0 ] || fail "dvbs on 1 s of input: exit $st (124: not within 8 s): $(tail -n 3 "$d/dvbs-cut.err")"
dvbscheck dvbs-cut "$dv_rate" dvbs2 || fail "dvbs on 1 s of input"
# without --width, the receiver's own search finds the carrier too; a --width narrower than the carrier (3 MHz, for its
# 6.75 MHz) leaves the receiver no carrier it can lock to
cp "$d/dvbs.gen" "$d/dvbs-own.gen"
"$bin" --format cf32 --mode dvbs --rate "$dv_rate" <"$d/dvbs.iq" >"$d/dvbs-own.jsonl" 2>"$d/dvbs-own.err" ||
  fail "dvbs without --width: $(tail -n 3 "$d/dvbs-own.err")"
ln -f "$d/dvbs.iq" "$d/dvbs-own.iq"
dvbscheck dvbs-own "$dv_rate" dvbs2 || fail "dvbs without --width"
"$bin" --format cf32 --mode dvbs --rate "$dv_rate" --width 3000000 <"$d/dvbs.iq" >"$d/dvbs-narrow.jsonl" 2>"$d/dvbs-narrow.err" ||
  fail "dvbs --width 3000000: $(tail -n 3 "$d/dvbs-narrow.err")"
python3 -I -c '
import json, sys
ls = [json.loads(l) for l in open(sys.argv[1], "rb")]
assert ls and not any(o["locked"] for o in ls), f"--width 3000000 on a 6.75 MHz carrier: {ls!r}"
print(f"ok: dvbs --width 3000000 on the 5 Msym/s carrier: {len(ls)} line(s), none locked")' "$d/dvbs-narrow.jsonl" ||
  fail "dvbs with a --width narrower than the carrier"
pcap dvbs 101 --mode dvbs --rate "$dv_rate" --width "$dv_width" || fail "dvbs on the pcap port"
# MPE (DVB-S) and ULE (DVB-S2, a transport stream): the receiver's transport stream handed back to it (feedTs), at 4 Msps
for e in mpe ule; do
  gen "dvbs-$e" --mode dvbs --rate 4000000 --encap "$e"
  rx "dvbs-$e" --mode dvbs --rate 4000000
  dvbscheck "dvbs-$e" 4000000 "$([ "$e" = mpe ] && echo dvbs || echo dvbs2)" || fail "dvbs with $e"
  pcap "dvbs-$e" 101 --mode dvbs --rate 4000000 || fail "dvbs with $e on the pcap port"
done
rm -f "$d"/dvbs*.iq

# --- the polled path's read-ahead (src/poll.cpp, ReadAhead) is sized per mode. A mode whose session never waits (dvbt) keeps
# its footprint from a file: its peak RSS within 64 MB of master 9843b13's build on the same run, 32480 kB (measured
# 2026-11-24 on the build host; the round-0 read-ahead of 4 s held 345 MB). gsmr, whose session waits, reads ahead two
# stretches: within 256 MB of the build before the read-ahead (0cf4726), 107824 kB, measured the same way
"$gen" --mode dvbt --rate 10000000 >"$d/dvbt10.iq" 2>"$d/dvbt10.gen" || fail "onair-gen --mode dvbt: $(cat "$d/dvbt10.gen")"
/usr/bin/time -f %M -o "$d/rss" "$bin" --mode dvbt --rate 10000000 <"$d/dvbt10.iq" >/dev/null 2>"$d/dvbt10.err" ||
  fail "dvbt at 10 MHz: $(tail -n 3 "$d/dvbt10.err")"
rm -f "$d/dvbt10.iq"
rss=$(cat "$d/rss")
[ "$rss" -le $((32480 + 64 * 1024)) ] || fail "dvbt at 10 MHz from a file: peak RSS $rss kB, master's 32480 kB"
echo "ok: dvbt at 10 MHz from a file: peak RSS $rss kB (master 9843b13: 32480 kB)"
# and within 8 MB of 6ee1931's (33328 kB at most, plan 47 Task 4): onair-rx links no more of OnAir than its modes need (its
# DVB-S receiver's rrcAt is the wrapper's own, src/rrc.cpp, so OnAir's signal generator and FFmpeg stay out)
[ "$rss" -le $((33328 + 8 * 1024)) ] || fail "dvbt at 10 MHz from a file: peak RSS $rss kB, 6ee1931's 33328 kB"
echo "ok: dvbt at 10 MHz from a file: within 8 MB of 6ee1931's 33328 kB"
# onair-rx needs none of FFmpeg's libraries (onair-gen, which links OnAir's generators, does)
if command -v ldd >/dev/null; then
  ff=$(ldd "$bin" | grep -E 'libavformat|libavcodec|libavutil|libswscale|libswresample' || true)
  [ -z "$ff" ] || fail "onair-rx links FFmpeg: $ff"
  echo "ok: onair-rx needs no FFmpeg library (ldd)"
else
  echo "skip: no ldd, onair-rx's FFmpeg libraries not checked"
fi
/usr/bin/time -f %M -o "$d/rss" "$bin" --mode gsmr --rate 2000000 --freq "$gr_hz" <"$d/gsmr.iq" >/dev/null 2>"$d/gsmr-rss.err" ||
  fail "gsmr for its RSS: $(tail -n 3 "$d/gsmr-rss.err")"
rss=$(cat "$d/rss")
[ "$rss" -le $((107824 + 256 * 1024)) ] || fail "gsmr from a file: peak RSS $rss kB, 107824 kB before the read-ahead"
echo "ok: gsmr from a file: peak RSS $rss kB (107824 kB before the read-ahead)"
# --drop: the read-ahead never waits. A writer at 8 times real time, with the session slowed to 5 ms a block
# (ONAIR_RX_TEST_BLOCK_DELAY_US, a test knob, so that the queue fills: under --drop it holds gsmr's two stretches, 4.4 s),
# is never held up 0.2 s; the blocks that find the queue full are dropped (whole blocks, and the short last one) and said
# on stderr, and t_sample stays a block's index on the input
python3 -I - "$bin" "$d/gsmr.iq" "$gr_hz" <<'EOF' || fail "gsmr --drop, a writer at 8 times real time"
import json, os, re, subprocess, sys, threading, time
binary, iq, hz = sys.argv[1:4]
rate, chunk = 8 * 2000000.0, 16384 * 8
data = open(iq, "rb").read()
env = dict(os.environ, ONAIR_RX_TEST_BLOCK_DELAY_US="5000")
p = subprocess.Popen([binary, "--format", "cf32", "--mode", "gsmr", "--rate", "2000000", "--freq", hz, "--drop"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
got = {}
ths = [threading.Thread(target=lambda k=k, f=f: got.__setitem__(k, f.read())) for k, f in (("out", p.stdout), ("err", p.stderr))]
for th in ths: th.start()
t0, worst = time.monotonic(), 0.0
for i in range(0, len(data), chunk):
    due = t0 + (i / 8) / rate
    now = time.monotonic()
    if now < due:
        time.sleep(due - now)
    w = time.monotonic()
    p.stdin.write(data[i:i + chunk]); p.stdin.flush()
    worst = max(worst, time.monotonic() - w)
p.stdin.close()
for th in ths: th.join(60)
p.wait(60)
err = got["err"].decode(errors="replace")
assert p.returncode == 0, f"exit {p.returncode}: {err}"
g = re.search(r"onair-rx: gsmr: (\d+) samples dropped", err)
assert g and int(g.group(1)) > 0, \
    f"no dropped line ({time.monotonic() - t0:.2f} s, writes up to {worst:.3f} s): {err}"
nsamp = len(data) // 8
for l in got["out"].splitlines():
    t = json.loads(l)["t_sample"]
    assert t % 16384 == 0 and 0 <= t < nsamp, f"t_sample {t}"
print(f"stall: gsmr --drop at 8 times real time: the writer held up at most {worst * 1000:.1f} ms by a write; "
      f"{g.group(1)} samples dropped")
assert worst < 0.2, f"a write held the writer up {worst:.3f} s"
EOF

# --- noise alone: 8 s (Wi-Fi: 2 s, LTE: 1 s) of /dev/urandom as cs16 gives no line and exit 0
for row in "zigbee 4000000 $zb_hz 8" "thread 4000000 $zb_hz 8" "zwave 2000000 $zw_hz 8" "wifi 20000000 $wf_hz 2" \
  "wisun 2000000 $ws_hz 8" "tetra 2000000 $te_hz 8" "gsmr 2000000 $gr_hz 8" "lte $lte_rate $lte_hz 1"; do
  read -r m rate hz secs <<<"$row"
  head -c $((rate * secs * 4)) /dev/urandom >"$d/noise.cs16"
  /usr/bin/time -f %U -o "$d/noise.cpu" "$bin" --mode "$m" --rate "$rate" --freq "$hz" --format cs16 <"$d/noise.cs16" \
    >"$d/noise.jsonl" 2>"$d/noise.err" || fail "$m on noise: onair-rx exited $?: $(tail -n 3 "$d/noise.err")"
  [ ! -s "$d/noise.jsonl" ] || fail "$m: a line on noise: $(head -n 1 "$d/noise.jsonl")"
  echo "ok: $m on $secs s of noise: no line, exit 0 ($(cat "$d/noise.cpu") s user over $secs s of input)"
done
rm -f "$d/noise.cs16"
# dvbs on 6 s of noise, through a pipe: a status line at 2 s and at least every 2 s after, none locked
head -c $((dv_rate * 6 * 4)) /dev/urandom | "$bin" --mode dvbs --rate "$dv_rate" --width "$dv_width" --format cs16 \
  >"$d/dvbs-noise.jsonl" 2>"$d/dvbs-noise.err" || fail "dvbs on noise: onair-rx exited $?: $(tail -n 3 "$d/dvbs-noise.err")"
dvbscheck dvbs-noise "$dv_rate" noise $((dv_rate * 6)) || fail "dvbs on noise"

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
