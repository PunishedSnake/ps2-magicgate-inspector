#!/usr/bin/env python3
"""Instrument PS2SDK 2.0 SECRMAN 1.4 without changing successful semantics.

The emitted failure record intentionally matches the compatibility SECRMAN
record so both backends can be compared with one EE-side decoder.

PS2SDK 2.0 normally routes GET_KBIT through the private scePreEncryptKbit()
helper. The diagnostic build must distinguish the two Mechacon half-key calls,
so it expands that helper inline inside SecrDownloadGetKbit(). The now-unused
static helper and its forward declaration are removed from the temporary source
tree to keep PS2SDK's -Werror build clean.

This patch is applied to a pinned temporary PS2SDK checkout during CI. It does
not vendor or permanently fork upstream SECRMAN source in this repository.
"""

import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1])
secrman_path = ROOT / "src" / "secrman.c"
cardauth_path = ROOT / "src" / "CardAuth.c"


def find_function(text: str, signature_start: str):
    start = text.find(signature_start)
    if start < 0:
        raise SystemExit(f"missing function: {signature_start}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"missing opening brace: {signature_start}")
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
    raise SystemExit(f"unterminated function: {signature_start}")


def replace_function(text: str, signature_start: str, replacement: str):
    start, end = find_function(text, signature_start)
    return text[:start] + replacement + text[end:]


def instrument_send(text: str, signature_start: str, id_expr: str,
                    status_expr: str, read_checksums=False):
    start, end = find_function(text, signature_start)
    func = text[start:end]
    needle = "if (SendMcCommand(port, slot, &sio2packet) == 0) {"
    if needle not in func:
        raise SystemExit(f"missing SendMcCommand pattern in {signature_start}")

    capture = (
        "mgdiag_command = (unsigned char)command;\n"
        "    mgdiag_transfer_rc = SendMcCommand(port, slot, &sio2packet);\n"
        "    mgdiag_stat6c = sio2packet.stat6c;\n"
        f"    mgdiag_id = {id_expr};\n"
        f"    mgdiag_status = {status_expr};\n"
    )
    if read_checksums:
        capture += (
            "    mgdiag_read_checksum = rdbuf[12];\n"
            "    mgdiag_calc_checksum = calculate_sio2_buffer_checksum(&rdbuf[4], 8);\n"
        )
    capture += "\n    if (mgdiag_transfer_rc == 0) {"
    func = func.replace(needle, capture, 1)
    return text[:start] + func + text[end:]


card = cardauth_path.read_text()
include_marker = '#include "CardAuth.h"\n'
if include_marker not in card:
    raise SystemExit("CardAuth include marker not found")
card = card.replace(include_marker, include_marker + r'''
/* PS2 Memory Card Inspector build-time diagnostics. Capture the transfer that
 * belongs to the real GET_KBIT CardAuth path; no command is replayed. */
extern volatile int mgdiag_command;
extern volatile int mgdiag_transfer_rc;
extern volatile unsigned int mgdiag_stat6c;
extern volatile unsigned char mgdiag_id;
extern volatile unsigned char mgdiag_status;
extern volatile unsigned char mgdiag_read_checksum;
extern volatile unsigned char mgdiag_calc_checksum;
''', 1)

card = instrument_send(card, "int card_auth_write(", "rdbuf[12]", "rdbuf[13]")
card = instrument_send(card, "int card_auth_read(", "rdbuf[3]", "rdbuf[13]", True)
card = instrument_send(card, "int card_auth(", "rdbuf[3]", "rdbuf[4]")
cardauth_path.write_text(card)

secr = secrman_path.read_text()
insert_marker = "extern struct irx_export_table _exp_secrman;\n"
if insert_marker not in secr:
    raise SystemExit("SECRMAN export marker not found")
secr = secr.replace(insert_marker, insert_marker + r'''

/* Failed-GET_KBIT diagnostic state. Successful SECRMAN behavior is unchanged;
 * this state is serialized into the otherwise unusable Kbit reply on failure. */
volatile int mgdiag_command;
volatile int mgdiag_transfer_rc;
volatile unsigned int mgdiag_stat6c;
volatile unsigned char mgdiag_id;
volatile unsigned char mgdiag_status;
volatile unsigned char mgdiag_read_checksum;
volatile unsigned char mgdiag_calc_checksum;

static int MgDiagAnyNonzero(const unsigned char *p, int size)
{
    int i;
    for (i = 0; i < size; i++)
        if (p[i] != 0)
            return 1;
    return 0;
}

static void MgDiagClearCard(void)
{
    mgdiag_command = -1;
    mgdiag_transfer_rc = -128;
    mgdiag_stat6c = 0;
    mgdiag_id = 0;
    mgdiag_status = 0;
    mgdiag_read_checksum = 0;
    mgdiag_calc_checksum = 0;
}

static unsigned char MgDiagCardReason(void)
{
    if (mgdiag_command < 0) return 6;
    if (mgdiag_transfer_rc == 0) return 1;
    if (((mgdiag_stat6c >> 13) & 1) || ((mgdiag_stat6c >> 14) & 3)) return 2;
    if (mgdiag_id != 0x2B) return 3;
    if (mgdiag_status == 0x66) return 4;
    if (mgdiag_command == 0x53 && mgdiag_read_checksum != mgdiag_calc_checksum) return 5;
    return 7;
}

static void MgDiagEncodeFailure(void *kbit, unsigned char stage,
                                unsigned char pre0, unsigned char pre1,
                                unsigned char mecha0, unsigned char mecha1)
{
    unsigned char *d = (unsigned char *)kbit;
    unsigned int s = mgdiag_stat6c;
    d[0] = 0xD2;
    d[1] = 0x12;
    d[2] = stage; /* 1/2 = Mechacon half 0/1, 3/4 = CardAuth half 0/1 */
    d[3] = (mgdiag_command < 0) ? 0xFF : (unsigned char)mgdiag_command;
    d[4] = (stage >= 3) ? MgDiagCardReason() : 0;
    d[5] = (unsigned char)mgdiag_transfer_rc;
    d[6] = mgdiag_id;
    d[7] = mgdiag_status;
    d[8] = (unsigned char)(s & 0xFF);
    d[9] = (unsigned char)((s >> 8) & 0xFF);
    d[10] = (unsigned char)((s >> 16) & 0xFF);
    d[11] = (unsigned char)((s >> 24) & 0xFF);
    d[12] = pre0;
    d[13] = pre1;
    d[14] = mecha0;
    d[15] = mecha1;
}
''', 1)

# Expanding the helper below makes the failed half observable. Remove the stock
# helper so PS2SDK 2.0's -Werror build does not reject it as unused.
prototype = "static int scePreEncryptKbit(void *kbit);\n"
if prototype not in secr:
    raise SystemExit("scePreEncryptKbit prototype not found")
secr = secr.replace(prototype, "", 1)
secr = replace_function(secr, "static int scePreEncryptKbit(", "")

new_get_kbit = r'''int SecrDownloadGetKbit(int port, int slot, void *kbit)
{
    int mecha0, mecha1;
    unsigned char pre0, pre1;

    MgDiagClearCard();
    mecha0 = _PreEncryptKbit1(kbit);
    pre0 = (unsigned char)MgDiagAnyNonzero((const unsigned char *)kbit, 8);
    if (mecha0 == 0) {
        MgDiagEncodeFailure(kbit, 1, pre0, 0, 0, 0);
        return 0;
    }

    mecha1 = _PreEncryptKbit2((void *)((unsigned char *)kbit + 8));
    pre1 = (unsigned char)MgDiagAnyNonzero((const unsigned char *)kbit + 8, 8);
    if (mecha1 == 0) {
        MgDiagEncodeFailure(kbit, 2, pre0, pre1, 1, 0);
        return 0;
    }

    MgDiagClearCard();
    if (card_encrypt(port, slot, kbit) == 0) {
        MgDiagEncodeFailure(kbit, 3, pre0, pre1, 1, 1);
        return 0;
    }

    MgDiagClearCard();
    if (card_encrypt(port, slot, (void *)((unsigned char *)kbit + 8)) == 0) {
        MgDiagEncodeFailure(kbit, 4, pre0, pre1, 1, 1);
        return 0;
    }

    return 1;
}'''
secr = replace_function(secr, "int SecrDownloadGetKbit(", new_get_kbit)
secrman_path.write_text(secr)

print("PS2SDK SECRMAN 1.4 MagicGate instrumentation applied")
