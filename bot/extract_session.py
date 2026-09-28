#!/usr/bin/env python3
"""Extract Lords Mobile login credentials (igg_id, device_uuid, session JWT)
from a PCAPdroid pcap capture of the game's login connection (TCP port 5999).

Login packet layout (582 bytes, little-endian, plain TCP):
  0   u16 packet length (582)
  2   u16 packet type (0x0413 LOGINTOL / 0x0414 LOGINTOP)
  4   u64 igg_id
  12  u8  version_minor, u8 version_major, u16 version_patch
  16  u8  1, u8 language_code
  18  50 bytes device_uuid (nul-padded string)
  68  u16 session_len
  70  session token (JWT, up to 512 bytes)
"""
import struct
import sys
import re

PORT = 5999
PKT_LEN = 582
TYPES = (0x0413, 0x0414)


def read_pcap(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"\xd4\xc3\xb2\xa1" or data[:4] == b"\xa1\xb2\xc3\xd4":
        endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
        off = 24
        while off + 16 <= len(data):
            _, _, caplen, _ = struct.unpack_from(endian + "IIII", data, off)
            off += 16
            yield data[off:off + caplen]
            off += caplen
    elif data[:4] == b"\x0a\x0d\x0d\x0a":  # pcapng
        block = None
        off = 0
        endian = "<"
        while off + 12 <= len(data):
            btype = struct.unpack_from("<I", data, off)[0]
            if btype == 0x0A0D0D0A:
                blen = struct.unpack_from("<I", data, off + 4)[0]
                bom = data[off + 8:off + 12]
                endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
                blen = struct.unpack_from(endian + "I", data, off + 4)[0]
            else:
                blen = struct.unpack_from(endian + "I", data, off + 4)[0]
            if blen < 12 or off + blen > len(data):
                break
            block = data[off:off + blen]
            if btype == 6:  # Enhanced Packet Block
                caplen = struct.unpack_from(endian + "I", block, 20)[0]
                yield block[28:28 + caplen]
            off += blen
    else:
        sys.exit("not a pcap/pcapng file")


def ip_tcp_payload(pkt):
    if len(pkt) < 14:
        return None
    ethertype = struct.unpack_from("!H", pkt, 12)[0]
    if ethertype == 0x0800:
        ipoff = 14
    elif ethertype == 0x86DD:
        return None
    else:  # maybe raw IP (no link layer)
        ipoff = 0
    if len(pkt) < ipoff + 20:
        return None
    vihl = pkt[ipoff]
    if vihl >> 4 != 4:
        return None
    ihl = (vihl & 0xF) * 4
    proto = pkt[ipoff + 9]
    if proto != 6:
        return None
    total_len = struct.unpack_from("!H", pkt, ipoff + 2)[0]
    tcpoff = ipoff + ihl
    if len(pkt) < tcpoff + 20:
        return None
    sport, dport = struct.unpack_from("!HH", pkt, tcpoff)
    doff = (pkt[tcpoff + 12] >> 4) * 4
    payload = pkt[tcpoff + doff:ipoff + total_len]
    if not payload:
        return None
    return sport, dport, payload


def try_parse(payload):
    if len(payload) < 70:
        return None
    plen, ptype = struct.unpack_from("<HH", payload, 0)
    if plen != PKT_LEN or ptype not in TYPES:
        return None
    igg_id = struct.unpack_from("<Q", payload, 4)[0]
    uuid_raw = payload[18:68].split(b"\x00")[0].decode("ascii", "replace")
    slen = struct.unpack_from("<H", payload, 68)[0]
    session = payload[70:70 + min(slen, 512)].decode("ascii", "replace")
    if not re.match(r"^eyJ", session):
        return None
    return igg_id, uuid_raw, session, ptype


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: extract_session.py capture.pcap")
    found = []
    for pkt in read_pcap(sys.argv[1]):
        ip = ip_tcp_payload(pkt)
        if not ip:
            continue
        sport, dport, payload = ip
        if sport != PORT and dport != PORT:
            continue
        res = try_parse(payload)
        if res:
            found.append(res)
    if not found:
        sys.exit("No 582-byte login packet found on port 5999. "
                 "Make sure the capture includes the game loading.")
    igg_id, uuid, session, ptype = found[-1]  # use the newest one
    print("packet type : 0x%04X" % ptype)
    print("igg_id      : %d" % igg_id)
    print("device_uuid : %s" % uuid)
    print("access_key  : %s" % session)
    print()
    print("Paste into config.cfg:")
    print("account.igg_id = %d" % igg_id)
    print("account.device_uuid = %s" % uuid)
    print("account.access_key = %s" % session)


if __name__ == "__main__":
    main()
