#!/usr/bin/env python3
"""Patch PS2SDK legacy MCSERV for Drebin raw-page access.

PS2SDK's legacy MCSERV receives logical EE memory-card ports 0/1. Its normal
GetInfo path initializes the complete MCMAN device state (card type, page size,
block size, flags and filesystem geometry) under those logical indices.

The historical raw erase/read/write helpers then remap 0/1 to 2/3 before calling
MCMAN. That is unnecessary with the PS2SDK MCMAN used by Drebin: MCMAN's SIO2
packet builder already maps `(port & 1)` onto the physical memory-card channels
2/3. Remapping inside MCSERV therefore switches the raw operation to a different
mcman_devinfos[] entry which has not received the complete GetInfo/device-spec
initialization. Merely detecting a card in 2/3 is insufficient because raw page
I/O immediately consumes pagesize/cardflags from that structure.

Drebin patches only the temporary non-XMC MCSERV built for Card Tools:
- preserve logical port 0/1 for erase/read/write MCMAN calls;
- let MCMAN itself select physical SIO2 channel 2/3;
- propagate the real McReadPage result instead of hard-coding success.

The normal Sony ROM X stack and PS2SDK X-style MagicGate stack are untouched.
"""

from pathlib import Path
import sys


def patch(path: Path) -> None:
    text = path.read_text()

    # Both _McEraseBlock and _McReadPage historically rewrite the logical EE
    # port into a physical SIO2 channel before consulting MCMAN state. Keep the
    # already initialized logical state instead. MCMAN's sio2packet_add() does
    # the physical `(port & 1) + 2` selection when it actually talks to SIO2.
    old = "\tdP->port = (dP->port & 1) + 2;\n\n\tif (McGetMcType(dP->port, dP->slot) == 2) {"
    new = """\t/*
\t * Keep logical port 0/1 here. mcGetInfo() initialized the complete MCMAN
\t * device structure at that index, while MCMAN itself maps the low port bit
\t * to physical SIO2 memory-card channel 2/3 when building the transfer.
\t */
\tif (McGetMcType(dP->port, dP->slot) == 2) {"""

    count = text.count(old)
    if count != 2:
        raise SystemExit(
            f"expected 2 legacy erase/read remap sites in {path}, found {count}"
        )
    text = text.replace(old, new)

    # Upstream legacy MCSERV discards the actual low-level page-read result.
    old_read = """\tif (McGetMcType(dP->port, dP->slot) == 2) {
\t\tr = 0;
\t\tMcReadPage(dP->port, dP->slot, dP->fd, (void *)(mcserv_buf + fastsize));
\t}"""
    new_read = """\tif (McGetMcType(dP->port, dP->slot) == 2) {
\t\tr = McReadPage(dP->port, dP->slot, dP->fd,
\t\t               (void *)(mcserv_buf + fastsize));
\t}"""
    if old_read not in text:
        raise SystemExit("legacy _McReadPage body did not match expected PS2SDK source")
    text = text.replace(old_read, new_read, 1)

    # _McWritePage applies the same 0/1 -> 2/3 remap inline in its return call.
    old_write = """\treturn McWritePage((dP->port & 1) | 2, dP->slot, dP->fd, (void *)(mcserv_buf + fastsize), eccbuf);"""
    new_write = """\treturn McWritePage(dP->port, dP->slot, dP->fd,
\t                   (void *)(mcserv_buf + fastsize), eccbuf);"""
    if old_write not in text:
        raise SystemExit("legacy _McWritePage port remap did not match expected PS2SDK source")
    text = text.replace(old_write, new_write, 1)

    path.write_text(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_raw_mcserv_ports.py <mcserv.c>")
    source = Path(sys.argv[1])
    patch(source)
    print("PS2SDK legacy MCSERV logical-state raw-page patch applied")
