#!/usr/bin/env python3
"""Patch PS2SDK legacy MCSERV for Drebin raw-page access.

PS2SDK's old MCSERV receives logical EE ports 0/1. Its filesystem GetInfo path
keeps MCMAN state under those logical indices, while the raw erase/read helpers
translate the same port to physical SIO2 memory-card channels 2/3 before asking
McGetMcType(). On a freshly initialized MCMAN this leaves mcman_devinfos[2/3]
uninitialized, so raw page 0 falls into the PS1/PDA path and returns
sceMcResNoEntry (-4).

Drebin only patches the temporary legacy MCSERV built for Card Tools. The normal
Sony ROM X stack and the PS2SDK X-style MagicGate stack remain untouched.
"""

from pathlib import Path
import sys


def patch(path: Path) -> None:
    text = path.read_text()

    old = "\tdP->port = (dP->port & 1) + 2;\n\n\tif (McGetMcType(dP->port, dP->slot) == 2) {"
    new = """\tdP->port = (dP->port & 1) + 2;

\t/*
\t * Legacy raw-page RPCs use physical SIO2 memory-card channels 2/3,
\t * while the preceding EE mcGetInfo() populated logical-port state 0/1.
\t * Prime the physical MCMAN slot once, then keep the cheap cached type test
\t * for subsequent pages/blocks. Changed-card (-1) and no-format (-2) are
\t * valid here because raw imaging must also work on unformatted cards.
\t */
\tif (McGetMcType(dP->port, dP->slot) != 2) {
\t\tr = McDetectCard(dP->port, dP->slot);
\t\tif (r < sceMcResNoFormat)
\t\t\treturn r;
\t}

\tif (McGetMcType(dP->port, dP->slot) == 2) {"""

    count = text.count(old)
    if count != 2:
        raise SystemExit(
            f"expected 2 legacy erase/read port-state sites in {path}, found {count}"
        )
    text = text.replace(old, new)

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

    path.write_text(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_raw_mcserv_ports.py <mcserv.c>")
    source = Path(sys.argv[1])
    patch(source)
    print("PS2SDK legacy MCSERV raw-port state patch applied")
