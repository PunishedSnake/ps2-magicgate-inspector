#!/usr/bin/env python3
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
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1
    raise SystemExit(f"unterminated function: {signature_start}")


def replace_function(text: str, signature_start: str, replacement: str):
    start, end = find_function(text, signature_start)
    return text[:start] + replacement + text[end:]


def instrument_send(text: str, signature_start: str, id_expr: str, status_expr: str, read_checksums=False):
    start, end = find_function(text, signature_start)
    func = text[start:end]
    needle = "if(SendMcCommand(port, slot, &sio2packet)!=0){"
    if needle not in func:
        raise SystemExit(f"missing SendMcCommand pattern in {signature_start}")
    capture = (
        "dev12_diag_command=(unsigned char)command;\n"
        "\tdev12_diag_transfer_rc=SendMcCommand(port, slot, &sio2packet);\n"
        "\tdev12_diag_stat6c=sio2packet.stat6c;\n"
        f"\tdev12_diag_id={id_expr};\n"
        f"\tdev12_diag_status={status_expr};\n"
    )
    if read_checksums:
        capture += (
            "\tdev12_diag_read_checksum=rdbuf[12];\n"
            "\tdev12_diag_calc_checksum=calculate_sio2_buffer_checksum(&rdbuf[4], 8);\n"
        )
    capture += "\n\tif(dev12_diag_transfer_rc!=0){"
    func = func.replace(needle, capture, 1)
    return text[:start] + func + text[end:]


card = cardauth_path.read_text()
include_marker = '#include "CardAuth.h"\n'
if include_marker not in card:
    raise SystemExit("CardAuth include marker not found")
card = card.replace(include_marker, include_marker + r'''
/* Briscoe dev12: exact in-path GET_KBIT diagnostics. These globals are
 * defined by the temporary diagnostic SECRMAN build and sampled only when
 * SecrDownloadGetKbit() fails. */
extern volatile int dev12_diag_command;
extern volatile int dev12_diag_transfer_rc;
extern volatile unsigned int dev12_diag_stat6c;
extern volatile unsigned char dev12_diag_id;
extern volatile unsigned char dev12_diag_status;
extern volatile unsigned char dev12_diag_read_checksum;
extern volatile unsigned char dev12_diag_calc_checksum;
''', 1)

card = instrument_send(card, "int card_auth_write(", "rdbuf[12]", "rdbuf[13]")
card = instrument_send(card, "int card_auth_read(", "rdbuf[3]", "rdbuf[13]", True)
card = instrument_send(card, "int card_auth(", "rdbuf[3]", "rdbuf[4]")
cardauth_path.write_text(card)

secr = secrman_path.read_text()
insert_marker = "static ModloadCb_d30 var_00003d30;\t//0x00003d30\n"
if insert_marker not in secr:
    raise SystemExit("SECRMAN globals marker not found")
secr = secr.replace(insert_marker, insert_marker + r'''

/* Briscoe dev12 diagnostic state. This build preserves normal successful
 * semantics. On GET_KBIT failure only, the returned 16-byte kbit buffer is
 * replaced with a compact diagnostic record consumed by the EE Inspector. */
volatile int dev12_diag_command;
volatile int dev12_diag_transfer_rc;
volatile unsigned int dev12_diag_stat6c;
volatile unsigned char dev12_diag_id;
volatile unsigned char dev12_diag_status;
volatile unsigned char dev12_diag_read_checksum;
volatile unsigned char dev12_diag_calc_checksum;

static int Dev12AnyNonzero(const unsigned char *p, int size)
{
    int i;
    for(i=0; i<size; i++) if(p[i]!=0) return 1;
    return 0;
}

static void Dev12ClearCardDiag(void)
{
    dev12_diag_command=-1;
    dev12_diag_transfer_rc=-128;
    dev12_diag_stat6c=0;
    dev12_diag_id=0;
    dev12_diag_status=0;
    dev12_diag_read_checksum=0;
    dev12_diag_calc_checksum=0;
}

static unsigned char Dev12CardReason(void)
{
    if(dev12_diag_command<0) return 6; /* mcCommand handler missing / no CardAuth call */
    if(dev12_diag_transfer_rc==0) return 1; /* MCMAN callback rejected transfer */
    if(((dev12_diag_stat6c>>13)&1) || ((dev12_diag_stat6c>>14)&3)) return 2; /* SIO2 */
    if(dev12_diag_id!=0x2B) return 3;
    if(dev12_diag_status==0x66) return 4;
    if(dev12_diag_command==0x53 && dev12_diag_read_checksum!=dev12_diag_calc_checksum) return 5;
    return 7;
}

static void Dev12EncodeFailure(void *kbit, unsigned char stage,
                               unsigned char pre0, unsigned char pre1,
                               unsigned char mecha0, unsigned char mecha1)
{
    unsigned char *d=(unsigned char*)kbit;
    unsigned int s=dev12_diag_stat6c;
    d[0]=0xD2;
    d[1]=0x12;
    d[2]=stage; /* 1=mecha half0, 2=mecha half1, 3=card half0, 4=card half1 */
    d[3]=(dev12_diag_command<0)?0xFF:(unsigned char)dev12_diag_command;
    d[4]=(stage>=3)?Dev12CardReason():0;
    d[5]=(unsigned char)dev12_diag_transfer_rc;
    d[6]=dev12_diag_id;
    d[7]=dev12_diag_status;
    d[8]=(unsigned char)(s&0xFF);
    d[9]=(unsigned char)((s>>8)&0xFF);
    d[10]=(unsigned char)((s>>16)&0xFF);
    d[11]=(unsigned char)((s>>24)&0xFF);
    d[12]=pre0;
    d[13]=pre1;
    d[14]=mecha0;
    d[15]=mecha1;
}
''', 1)

new_get_kbit = r'''int SecrDownloadGetKbit(int port, int slot, void *kbit){
    int mecha0, mecha1;
    unsigned char pre0, pre1;

    Dev12ClearCardDiag();
    mecha0=func_00001ce8(kbit);
    pre0=(unsigned char)Dev12AnyNonzero((const unsigned char*)kbit, 8);
    if(mecha0==0){
        Dev12EncodeFailure(kbit, 1, pre0, 0, 0, 0);
        return 0;
    }

    mecha1=func_00001d64((void*)((unsigned int)kbit+8));
    pre1=(unsigned char)Dev12AnyNonzero((const unsigned char*)kbit+8, 8);
    if(mecha1==0){
        Dev12EncodeFailure(kbit, 2, pre0, pre1, 1, 0);
        return 0;
    }

    Dev12ClearCardDiag();
    if(card_encrypt(port, slot, kbit)==0){
        Dev12EncodeFailure(kbit, 3, pre0, pre1, 1, 1);
        return 0;
    }

    Dev12ClearCardDiag();
    if(card_encrypt(port, slot, (void*)((unsigned int)kbit+8))==0){
        Dev12EncodeFailure(kbit, 4, pre0, pre1, 1, 1);
        return 0;
    }

    return 1;
}'''
secr = replace_function(secr, "int SecrDownloadGetKbit(", new_get_kbit)
secrman_path.write_text(secr)

print("dev12 SECRMAN instrumentation applied")
