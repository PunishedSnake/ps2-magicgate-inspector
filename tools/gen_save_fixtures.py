#!/usr/bin/env python3
"""Generate deterministic save-container fixtures for tests/test_save_codecs.c.

Independence matters: these encoders are written from the published layouts
(mymc ps2save.py, psdevwiki, nocash PSX specs), not from the C decoders, and
every PS2 container is round-tripped through mymc++ (pip install mymcplusplus)
as an external oracle. The MAX fixture is produced by mymc++'s own LZARI
encoder, so the C LZARI decoder is checked against a third-party encoder.

    python3 tools/gen_save_fixtures.py           # (re)write tests/fixtures
    python3 tools/gen_save_fixtures.py --check   # verify committed fixtures
"""

from __future__ import annotations

import argparse
import binascii
import io
import pathlib
import random
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "fixtures"

TOD_CREATED = bytes([0, 30, 15, 12, 8, 11]) + struct.pack("<H", 2026)
TOD_MODIFIED = bytes([0, 45, 20, 13, 8, 11]) + struct.pack("<H", 2026)
DIR_MODE = 0x8427
FILE_MODE = 0x8497


# ----------------------------------------------------------------- content

def fullwidth(text: str) -> bytes:
    out = bytearray()
    for ch in text:
        c = ord(ch)
        if "A" <= ch <= "Z":
            out += bytes([0x82, 0x60 + c - ord("A")])
        elif "0" <= ch <= "9":
            out += bytes([0x82, 0x4F + c - ord("0")])
        elif ch == " ":
            out += b"\x81\x40"
        else:
            out += ch.encode()
    return bytes(out)


def icon_sys() -> bytes:
    b = bytearray(964)
    b[0:4] = b"PS2D"
    title = fullwidth("DREBIN") + fullwidth("TEST SAVE")
    struct.pack_into("<H", b, 6, len(fullwidth("DREBIN")))
    b[0xC0:0xC0 + len(title)] = title
    b[0x104:0x104 + 8] = b"list.ico"
    return bytes(b)


def ps2_save():
    rng = random.Random(0xD2EB1A)
    files = [
        ("icon.sys", icon_sys()),
        ("DATA.BIN", (b"DREBIN-SAVE-BLOCK " * 700)[:12345]),
        ("RAND.BIN", bytes(rng.getrandbits(8) for _ in range(3001))),
        ("empty.dat", b""),
    ]
    return "BESLES-99999DREBIN", files


def ps1_body(blocks: int, title: str, seed: int) -> bytes:
    rng = random.Random(seed)
    body = bytearray(rng.getrandbits(8) for _ in range(8192 * blocks))
    body[0:2] = b"SC"
    body[2] = 0x11
    body[3] = blocks
    t = fullwidth(title)[:64]
    body[4:68] = t + bytes(64 - len(t))
    return bytes(body)


PS1_SAVES = [
    ("BESLES-12345SAVE01", ps1_body(1, "DREBIN PS1 ONE", 1)),
    ("BASLUS-00001RPGDATA", ps1_body(2, "RPG DATA 2", 2)),
]


# ------------------------------------------------------------- PS2 writers

def round_up(n: int, a: int) -> int:
    return (n + a - 1) // a * a


def psu_entry(mode, length, name: bytes) -> bytes:
    e = bytearray(512)
    struct.pack_into("<HHI8sII8sI", e, 0, mode, 0, length, TOD_CREATED, 0, 0,
                     TOD_MODIFIED, 0)
    e[64:64 + len(name)] = name
    return bytes(e)


def make_psu(dirname, files) -> bytes:
    out = bytearray()
    out += psu_entry(DIR_MODE, len(files) + 2, dirname.encode())
    out += psu_entry(DIR_MODE, 0, b".")
    out += psu_entry(DIR_MODE, 0, b"..")
    for name, data in files:
        out += psu_entry(FILE_MODE, len(data), name.encode())
        out += data + bytes(round_up(len(data), 1024) - len(data))
    return bytes(out)


def rc4_state():
    from mymcplusplus.save import format_codebreaker as cb
    return list(cb.PS2SAVE_CBS_RC4S)


def rc4(data: bytes) -> bytes:
    s = rc4_state()
    i = j = 0
    out = bytearray(data)
    for n in range(len(out)):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[n] ^= s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


def make_cbs(dirname, files) -> bytes:
    body = bytearray()
    for name, data in files:
        h = struct.pack("<8s8sIHHII32s", TOD_CREATED, TOD_MODIFIED, len(data),
                        FILE_MODE, 0, 0, 0, name.encode())
        body += h + data
    comp = rc4(zlib.compress(bytes(body), 9))
    title = b"DREBIN TEST SAVE"
    hlen = 92 + 32 + 140
    hdr = bytearray(hlen)
    struct.pack_into("<4sIIII32s8s8sIIIIII", hdr, 0, b"CFU\0", 0x1F40, hlen,
                     len(body), len(comp), dirname.encode(), TOD_CREATED,
                     TOD_MODIFIED, 0, 0, DIR_MODE, 0, 0, 0)
    hdr[92:92 + len(title)] = title
    return bytes(hdr) + comp


def sps_head(name: bytes, length: int, mode: int) -> bytes:
    swapped = ((mode & 0xFF) << 8) | (mode >> 8)
    return struct.pack("<H64sI8xH2x8s8s", 98, name, length, swapped,
                       TOD_CREATED, TOD_MODIFIED)


def long_string(b: bytes) -> bytes:
    return struct.pack("<I", len(b)) + b


def make_sps(dirname, files) -> bytes:
    payload = sps_head(dirname.encode(), len(files) + 2, DIR_MODE)
    for name, data in files:
        payload += sps_head(name.encode(), len(data), FILE_MODE) + data
    out = (b"\x0d\0\0\0SharkPortSave" + struct.pack("<I", 2) +
           long_string(dirname.encode()) + long_string(b"2026-11-08") +
           long_string(b"Drebin fixture") + struct.pack("<I", len(payload)) +
           payload)
    return out + struct.pack("<I", 0)  # checksum (ignored by readers)


def make_psv(dirname, files) -> bytes:
    n = len(files)
    hdr = struct.pack("<4sI40s8xII", b"\0VSP", 0, bytes(40), 0x2C, 2)
    base = len(hdr) + 0x28 + 0x38 + 0x3C * n
    infos = bytearray()
    blob = bytearray()
    pos = base
    for name, data in files:
        infos += struct.pack("<8s8sII32sI", TOD_CREATED, TOD_MODIFIED,
                             len(data), FILE_MODE, name.encode(), pos)
        blob += data
        pos += len(data)
    ps2hdr = struct.pack("<IIIIIIIIII", sum(len(d) for _, d in files),
                         base, len(files[0][1]), 0, 0, 0, 0, 0, 0, n)
    root = struct.pack("<8s8sII32s", TOD_CREATED, TOD_MODIFIED, n + 2,
                       DIR_MODE, dirname.encode())
    return hdr + ps2hdr + root + bytes(infos) + bytes(blob)


def make_max_with_mymc(dirname, files) -> bytes:
    from mymcplusplus import ps2mc_dir
    from mymcplusplus.save import format_max_drive, ps2save
    sf = ps2save.PS2SaveFile()
    tod = ps2mc_dir.unpack_tod(TOD_CREATED)
    sf.set_directory((DIR_MODE, 0, len(files), tod, 0, 0, tod, 0,
                      dirname.encode()))
    for i, (name, data) in enumerate(files):
        sf.set_file(i, (FILE_MODE, 0, len(data), tod, 0, 0, tod, 0,
                        name.encode()), data)
    f = io.BytesIO()
    format_max_drive.save(sf, f)
    return f.getvalue()


def mymc_load(fmt_module, blob):
    from mymcplusplus.save import ps2save, utils
    # mymc++ 3.x format_ems references read_fixed without importing it.
    for name in ("read_fixed", "read_struct", "read_long_string"):
        if not hasattr(fmt_module, name) and hasattr(utils, name):
            setattr(fmt_module, name, getattr(utils, name))
    sf = ps2save.PS2SaveFile()
    fmt_module.load(sf, io.BytesIO(blob))
    out = []
    for i in range(len(sf)):
        ent, data = sf.get_file(i)
        out.append((ent[8].decode(), bytes(data)))
    return sf.dirent[8].decode(), out


# ------------------------------------------------------------- PS1 writers

def frame_xor(frame: bytearray) -> None:
    x = 0
    for b in frame[:127]:
        x ^= b
    frame[127] = x


def ps1_card(saves, deleted_slot=None) -> bytes:
    card = bytearray(131072)
    hdr = bytearray(128)
    hdr[0:2] = b"MC"
    frame_xor(hdr)
    card[0:128] = hdr
    dirs = [bytearray(128) for _ in range(15)]
    for d in dirs:
        d[0] = 0xA0
        d[8:10] = b"\xff\xff"
    slot = 0
    for name, body in saves:
        n = len(body) // 8192
        chain = list(range(slot, slot + n))
        for k, s in enumerate(chain):
            d = bytearray(128)
            if k == 0:
                d[0] = 0x51
                struct.pack_into("<I", d, 4, len(body))
                d[10:10 + len(name)] = name.encode()
            else:
                d[0] = 0x53 if k == n - 1 else 0x52
            nxt = chain[k + 1] if k + 1 < n else 0xFFFF
            struct.pack_into("<H", d, 8, nxt)
            dirs[s] = d
            card[8192 * (1 + s):8192 * (2 + s)] = body[8192 * k:8192 * (k + 1)]
        slot += n
    if deleted_slot is not None:
        d = bytearray(128)
        d[0] = 0xA1
        struct.pack_into("<I", d, 4, 8192)
        struct.pack_into("<H", d, 8, 0xFFFF)
        d[10:10 + 18] = b"BESLES-77777DELETE"
        dirs[deleted_slot] = d
    for s, d in enumerate(dirs):
        frame_xor(d)
        card[128 * (1 + s):128 * (2 + s)] = d
    for i in range(20):
        f = bytearray(128)
        f[0:4] = b"\xff\xff\xff\xff"
        f[8:10] = b"\xff\xff"
        frame_xor(f)
        card[2048 + 128 * i:2048 + 128 * (i + 1)] = f
    card[8064:8192] = card[0:128]
    return bytes(card)


def make_gme(card: bytes) -> bytes:
    h = bytearray(3904)
    h[0:11] = b"123-456-STD"
    h[18] = 1
    h[20] = 1
    h[21] = ord("M")
    for s in range(15):
        h[22 + s] = card[128 * (1 + s)]
        h[38 + s] = card[128 * (1 + s) + 8]
    comment = b"Drebin GME comment"
    h[64:64 + len(comment)] = comment
    return bytes(h) + card


def make_vgs(card: bytes) -> bytes:
    h = bytearray(64)
    h[0:4] = b"VgsM"
    h[4] = h[8] = h[12] = 1
    h[17] = 2
    return bytes(h) + card


def make_vmp(card: bytes) -> bytes:
    h = bytearray(128)
    h[0:4] = b"\0PMV"
    h[4] = 0x80
    return bytes(h) + card  # signature left zero: import-only format


def make_mcs(name, body) -> bytes:
    d = bytearray(128)
    d[0] = 0x51
    struct.pack_into("<I", d, 4, len(body))
    struct.pack_into("<H", d, 8, 0xFFFF)
    d[10:10 + len(name)] = name.encode()
    frame_xor(d)
    return bytes(d) + body


def make_ar(name, body) -> bytes:
    h = bytearray(54)
    h[0:len(name)] = name.encode()
    title = b"AR TITLE"
    h[21:21 + len(title)] = title
    return bytes(h) + body


def make_psv1(name, body) -> bytes:
    hdr = struct.pack("<4sI40s8xII", b"\0VSP", 0, bytes(40), 0x14, 1)
    ps1 = struct.pack("<II20xI4x20s12x", len(body), 0x84, 0x3900,
                      name.encode())
    return hdr + ps1 + body


# ------------------------------------------------------------------- main

def build() -> dict[str, bytes]:
    from mymcplusplus.save import (format_codebreaker, format_ems,
                                   format_max_drive, format_psv,
                                   format_sharkport)
    dirname, files = ps2_save()
    out: dict[str, bytes] = {}
    out["ps2/save.psu"] = make_psu(dirname, files)
    out["ps2/save.max"] = make_max_with_mymc(dirname, files)
    out["ps2/save.cbs"] = make_cbs(dirname, files)
    out["ps2/save.sps"] = make_sps(dirname, files)
    out["ps2/save.psv"] = make_psv(dirname, files)

    # External oracle: every PS2 fixture must load in mymc++ unchanged.
    for path, mod in (("ps2/save.psu", format_ems), ("ps2/save.max", format_max_drive),
                      ("ps2/save.cbs", format_codebreaker),
                      ("ps2/save.sps", format_sharkport), ("ps2/save.psv", format_psv)):
        got_dir, got = mymc_load(mod, out[path])
        assert got_dir == dirname, (path, got_dir)
        assert got == files, path

    card = ps1_card(PS1_SAVES, deleted_slot=5)
    out["ps1/card.mcd"] = card
    out["ps1/card.gme"] = make_gme(card)
    out["ps1/card.vgs"] = make_vgs(card)
    out["ps1/card.vmp"] = make_vmp(card)
    name, body = PS1_SAVES[1]
    out["ps1/save.mcs"] = make_mcs(name, body)
    out["ps1/save.psx"] = make_ar(name, body)
    out["ps1/" + name + ".bin"] = body
    out["ps1/save1.psv"] = make_psv1(name, body)
    sf_dir, sf_files = mymc_load(format_psv, out["ps1/save1.psv"])
    assert sf_dir.rstrip("\0") == name and sf_files[0][1] == body

    # Manifest consumed by the C test: one line per expected file.
    lines = ["# kind name size crc32", f"ps2dir {dirname}"]
    for fname, data in files:
        lines.append(f"ps2file {fname} {len(data)} {binascii.crc32(data):08x}")
    for pname, pbody in PS1_SAVES:
        lines.append(f"ps1save {pname} {len(pbody)} {binascii.crc32(pbody):08x}")
    out["manifest.txt"] = ("\n".join(lines) + "\n").encode()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    try:
        data = build()
    except ImportError:
        print("mymcplusplus is required: pip install mymcplusplus", file=sys.stderr)
        return 2
    bad = 0
    for rel, blob in sorted(data.items()):
        p = OUT / rel
        if args.check:
            if not p.exists() or p.read_bytes() != blob:
                print(f"fixture differs: {rel}", file=sys.stderr)
                bad += 1
        else:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(blob)
    print(("checked " if args.check else "wrote ") + f"{len(data)} fixtures"
          + (", all match" if args.check and not bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
