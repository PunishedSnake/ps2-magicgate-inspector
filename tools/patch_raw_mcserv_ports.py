#!/usr/bin/env python3
"""Patch PS2SDK legacy MCSERV for Drebin raw-page access.

Port-numbering source of truth: docs/MEMORY_CARD_PORT_DOMAINS.md and
src/mc_port.h. The rule is not "always add 2" or "never add 2"; conversion
belongs exactly at the layer that crosses from logical card identity to a raw
SIO2 channel. Raw MCSERV is not that layer in Drebin.

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

There is one more legacy semantic that is hostile to an imaging tool. MCMAN's
McReadPage() retries a page whose spare-area ECC cannot be corrected. If the SIO2
page transfer itself succeeds on every attempt but ECC remains uncorrectable,
McReadPage() finally returns sceMcResNoFormat (-2) even though the last 512-byte
page payload is present in the destination buffer. A filesystem caller should
indeed distrust that page. A forensic/best-effort image dumper must preserve it
instead of deleting the whole partial image.

Drebin patches only the temporary non-XMC MCSERV built for Card Tools:
- preserve logical port 0/1 for erase/read/write MCMAN calls;
- let MCMAN itself select physical SIO2 channel 2/3;
- propagate the real McReadPage result;
- convert the special post-transfer uncorrectable-ECC result (-2) to positive 1;
- add private RPC command 0x81 which reads up to sixteen consecutive 512-byte
  pages into MCSERV's existing 8192-byte staging buffer and returns them to EE
  with one SIF DMA. Underlying MCMAN/SIO2 page semantics are unchanged and the
  public libmc ABI is untouched. The EE client falls back to stock mcReadPage if
  this extension is unavailable or a batch cannot be completed.

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
    # Preserve it, but turn MCMAN's special 'transport succeeded, ECC remained
    # uncorrectable after retries' result into a positive warning. _McReadPage
    # still DMA-copies the 512-byte payload to EE regardless of this status.
    old_read = """\tif (McGetMcType(dP->port, dP->slot) == 2) {
\t\tr = 0;
\t\tMcReadPage(dP->port, dP->slot, dP->fd, (void *)(mcserv_buf + fastsize));
\t}"""
    new_read = """\tif (McGetMcType(dP->port, dP->slot) == 2) {
\t\tr = McReadPage(dP->port, dP->slot, dP->fd,
\t\t               (void *)(mcserv_buf + fastsize));
\t\t/*
\t\t * McReadPage returns sceMcResNoFormat (-2) after repeated
\t\t * uncorrectable ECC even though mcman_readpage transferred the page.
\t\t * Return +1 so a raw image can keep that payload and report a warning.
\t\t */
\t\tif (r == sceMcResNoFormat)
\t\t\tr = 1;
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

    # Private Drebin bulk-read extension. MCSERV already owns an 8192-byte
    # staging buffer for normal file reads, so sixteen 512-byte NAND pages fit
    # without increasing IOP memory. The callback reads each page through the
    # same MCMAN routine as _McReadPage, preserves the historical per-page bad
    # block maintenance call, then performs one IOP->EE SIF DMA for the batch.
    # Result encoding: low 8 bits = pages DMA'd, bits 8..23 = ECC-warning mask.
    anchor = "//--------------------------------------------------------------\nvoid *cb_rpc_S_0400(u32 fno, void *buf, int size)\n"
    if anchor not in text:
        raise SystemExit("MCSERV RPC callback anchor not found")
    bulk_impl = r'''typedef struct DrebinBulkReadParam {
    int port;
    int slot;
    u32 start_page;
    u32 page_count;
    void *ee_buffer;
} DrebinBulkReadParam;

static int DrebinMcReadPages(void *rpc_buf)
{
    DrebinBulkReadParam *param = (DrebinBulkReadParam *)rpc_buf;
    SifDmaTransfer_t dma;
    unsigned int warning_mask = 0;
    unsigned int i;
    int intStatus;
    int dma_id;

    if (param == NULL || param->ee_buffer == NULL ||
        param->page_count == 0 || param->page_count > 16)
        return -1;
    if (McGetMcType(param->port, param->slot) != 2)
        return sceMcResFailDetect;

    for (i = 0; i < param->page_count; i++) {
        int r = McReadPage(param->port, param->slot,
                           param->start_page + i,
                           mcserv_buf + i * 512);

        /* Preserve the same maintenance point as one ordinary MCSERV callback
         * per page. This keeps bulk mode behavior equivalent to the legacy path
         * rather than silently changing MCMAN bad-block handling semantics. */
        McReplaceBadBlock();

        if (r == sceMcResNoFormat) {
            warning_mask |= 1u << i;
            continue;
        }
        if (r < 0)
            return r;
    }

    dma.src = mcserv_buf;
    dma.dest = param->ee_buffer;
    dma.size = param->page_count * 512;
    dma.attr = 0;

    CpuSuspendIntr(&intStatus);
    dma_id = sceSifSetDma(&dma, 1);
    CpuResumeIntr(intStatus);
    if (dma_id == 0)
        return -2;
    while (sceSifDmaStat(dma_id) >= 0)
        DelayThread(100);

    return (int)(param->page_count | (warning_mask << 8));
}

//--------------------------------------------------------------
'''
    text = text.replace(anchor, bulk_impl + anchor, 1)

    dispatch_anchor = """\t\tcase 0x80:\n#ifdef BUILDING_XMCSERV\n\t\tcase 0x11: // CMD_UNFORMAT\n#endif\n\t\t\trpc_stat.result = sceMcUnformat();\n\t\t\tbreak;"""
    dispatch_new = dispatch_anchor + """
#ifndef BUILDING_XMCSERV
\t\tcase 0x81: // DREBIN_PRIVATE_BULK_READ
\t\t\trpc_stat.result = DrebinMcReadPages(buf);
\t\t\t/* DrebinMcReadPages already preserves the legacy per-page
\t\t\t * McReplaceBadBlock maintenance point. */
\t\t\tneed_replace_bad_block = 0;
\t\t\tbreak;
#endif"""
    if dispatch_anchor not in text:
        raise SystemExit("MCSERV unformat dispatch anchor not found")
    text = text.replace(dispatch_anchor, dispatch_new, 1)

    path.write_text(text)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_raw_mcserv_ports.py <mcserv.c>")
    source = Path(sys.argv[1])
    patch(source)
    print("PS2SDK legacy MCSERV logical-state/ECC-tolerant/bulk-read patch applied")
