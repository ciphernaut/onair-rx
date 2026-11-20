-- OnAir: Wireshark helpers (written by OnAir, Wireshark window: "Add OnAir to Wireshark").
-- Mode S / ADS-B and Z-Wave have no dissector in Wireshark, so OnAir writes them under private link
-- types (USER0, USER1) and this file reads them. NMEA 0183 has a dissector that is off by default
-- on UDP: this file turns it on for the port OnAir uses for AIS. Described in docs/WIRESHARK-3.8.md.
local user0 = (wtap_encaps and wtap_encaps.USER0) or (wtap and wtap.USER0) or 45
local user1 = (wtap_encaps and wtap_encaps.USER1) or (wtap and wtap.USER1) or 46

-- USER0: a Mode S frame, 7 or 14 bytes, with its parity
local adsb = Proto("onair_modes", "Mode S / ADS-B (OnAir)")
local f_df = ProtoField.uint8("onair_modes.df", "Downlink format", base.DEC, nil, 0xF8)
local f_ca = ProtoField.uint8("onair_modes.ca", "Capability / third field", base.DEC, nil, 0x07)
local f_icao = ProtoField.uint24("onair_modes.icao", "Address (DF 11, 17, 18)", base.HEX)
local f_me = ProtoField.bytes("onair_modes.me", "Message (ME)")
local f_tc = ProtoField.uint8("onair_modes.tc", "Type code", base.DEC, nil, 0xF8)
local f_pi = ProtoField.uint24("onair_modes.parity", "Parity", base.HEX)
adsb.fields = { f_df, f_ca, f_icao, f_me, f_tc, f_pi }
function adsb.dissector(tvb, pinfo, tree)
    local n = tvb:captured_len()
    if n ~= 7 and n ~= 14 then return 0 end
    pinfo.cols.protocol = "Mode S"
    local t = tree:add(adsb, tvb())
    local df = math.floor(tvb(0, 1):uint() / 8)
    t:add(f_df, tvb(0, 1))
    t:add(f_ca, tvb(0, 1))
    if df == 11 or df == 17 or df == 18 then
        t:add(f_icao, tvb(1, 3))
        pinfo.cols.info = string.format("DF %d, %06X", df, tvb(1, 3):uint())
    else
        pinfo.cols.info = string.format("DF %d", df)
    end
    if n == 14 and (df == 17 or df == 18) then
        t:add(f_me, tvb(4, 7))
        t:add(f_tc, tvb(4, 1))
    end
    t:add(f_pi, tvb(n - 3, 3))
    return n
end
DissectorTable.get("wtap_encap"):add(user0, adsb)

-- USER1: one byte for the rate, then a Z-Wave MPDU
local zw = Proto("onair_zwave", "Z-Wave (OnAir)")
local rates = { [0] = "9.6 kbit/s", [1] = "40 kbit/s", [2] = "100 kbit/s" }
local z_rate = ProtoField.uint8("onair_zwave.rate", "Rate", base.DEC, rates)
local z_home = ProtoField.uint32("onair_zwave.home", "Home ID", base.HEX)
local z_src = ProtoField.uint8("onair_zwave.src", "Source node", base.DEC)
local z_rest = ProtoField.bytes("onair_zwave.rest", "Frame control, destination, payload and check (S0 / S2 payloads stay encrypted)")
zw.fields = { z_rate, z_home, z_src, z_rest }
function zw.dissector(tvb, pinfo, tree)
    if tvb:captured_len() < 6 then return 0 end
    pinfo.cols.protocol = "Z-Wave"
    local t = tree:add(zw, tvb())
    t:add(z_rate, tvb(0, 1))
    t:add(z_home, tvb(1, 4))
    t:add(z_src, tvb(5, 1))
    if tvb:captured_len() > 6 then t:add(z_rest, tvb(6)) end
    pinfo.cols.info = string.format("Home %08X, node %d", tvb(1, 4):uint(), tvb(5, 1):uint())
    return tvb:captured_len()
end
DissectorTable.get("wtap_encap"):add(user1, zw)

-- AIS: NMEA sentences in UDP, port 10110
local nmea = Dissector.get("nmea0183")
if nmea then DissectorTable.get("udp.port"):add(10110, nmea) end
