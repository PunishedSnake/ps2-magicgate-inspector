/* SPDX-License-Identifier: MIT */
/*
 * Content-based save/card container detection. Portable (host-tested).
 * Extension is only used to pick between variants that share one layout
 * (MAX vs PWS, SharkPort vs X-Port) and as a last-resort hint.
 */

#include <tamtypes.h>
#include <ctype.h>
#include <string.h>

#include "ps1_card.h"
#include "save_codec_ps2.h"
#include "save_transfer.h"

static int HasSuffix(const char *path, const char *suffix)
{
    size_t tl;
    size_t sl;
    size_t i;

    if (path == NULL)
        return 0;
    tl = strlen(path);
    sl = strlen(suffix);
    if (tl < sl)
        return 0;
    for (i = 0; i < sl; i++)
        if (tolower((unsigned char)path[tl - sl + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

int MciSaveTransferFormatImportable(MciSaveTransferFormat format)
{
    switch (format) {
        case MCI_SAVE_FORMAT_PSU:
        case MCI_SAVE_FORMAT_MAX:
        case MCI_SAVE_FORMAT_PWS:
        case MCI_SAVE_FORMAT_CBS:
        case MCI_SAVE_FORMAT_SPS:
        case MCI_SAVE_FORMAT_XPS:
        case MCI_SAVE_FORMAT_PSV_PS2:
        case MCI_SAVE_FORMAT_MCS_PS1:
        case MCI_SAVE_FORMAT_GME_PS1:
        case MCI_SAVE_FORMAT_PSX_PS1:
        case MCI_SAVE_FORMAT_PSV_PS1:
        case MCI_SAVE_FORMAT_IMAGE_PS1:
        case MCI_SAVE_FORMAT_RAW_PS1:
            return 1;
        default:
            return 0;
    }
}

MciSaveTransferFormat MciSaveTransferDetectBuffer(const unsigned char *prefix,
                                                  u32 prefix_size, u64 file_size,
                                                  const char *path,
                                                  int *generation)
{
    unsigned char window[4096];
    u32 size32;
    u32 off = 0u;
    MciPs1Container card;

    if (generation != NULL)
        *generation = 0;
    if (prefix == NULL || file_size == 0u)
        return MCI_SAVE_FORMAT_UNKNOWN;
    /* Detectors index at most ~3.9 KiB into the data; give them a zero-padded
     * window so a short prefix can never be over-read. */
    memset(window, 0, sizeof(window));
    memcpy(window, prefix, prefix_size < sizeof(window) ? prefix_size : sizeof(window));
    size32 = file_size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)file_size;

    switch (MciPs2DetectContainer(window, size32)) {
        case MCI_PS2_CONTAINER_MAX:
            if (generation) *generation = 2;
            return HasSuffix(path, ".pws") ? MCI_SAVE_FORMAT_PWS : MCI_SAVE_FORMAT_MAX;
        case MCI_PS2_CONTAINER_CBS:
            if (generation) *generation = 2;
            return MCI_SAVE_FORMAT_CBS;
        case MCI_PS2_CONTAINER_SPS:
            if (generation) *generation = 2;
            return HasSuffix(path, ".xps") ? MCI_SAVE_FORMAT_XPS : MCI_SAVE_FORMAT_SPS;
        case MCI_PS2_CONTAINER_PSV:
            if (generation) *generation = 2;
            return MCI_SAVE_FORMAT_PSV_PS2;
        case MCI_PS2_CONTAINER_PSU:
            if (generation) *generation = 2;
            return MCI_SAVE_FORMAT_PSU;
        default:
            break;
    }

    card = MciPs1DetectCardContainer(window, size32, &off);
    if (card != MCI_PS1_CONTAINER_NONE) {
        if (generation) *generation = 1;
        return card == MCI_PS1_CONTAINER_GME ? MCI_SAVE_FORMAT_GME_PS1
                                              : MCI_SAVE_FORMAT_IMAGE_PS1;
    }

    /* Single PS1 saves: the detector needs the real size for the 8 KiB
     * multiple check but reads at most the first 0x86 bytes. */
    switch (MciPs1DetectSingle(window, size32)) {
        case MCI_PS1_SINGLE_MCS:
            if (generation) *generation = 1;
            return MCI_SAVE_FORMAT_MCS_PS1;
        case MCI_PS1_SINGLE_AR:
            if (generation) *generation = 1;
            return MCI_SAVE_FORMAT_PSX_PS1;
        case MCI_PS1_SINGLE_PSV:
            if (generation) *generation = 1;
            return MCI_SAVE_FORMAT_PSV_PS1;
        case MCI_PS1_SINGLE_RAW:
            if (generation) *generation = 1;
            return MCI_SAVE_FORMAT_RAW_PS1;
        default:
            break;
    }

    /* PS2 card images keep their historical extension-based routing. */
    if (HasSuffix(path, ".ps2"))
        return MCI_SAVE_FORMAT_IMAGE_PS2;
    if (HasSuffix(path, ".vmc"))
        return MCI_SAVE_FORMAT_IMAGE_VMC;
    return MCI_SAVE_FORMAT_UNKNOWN;
}

const char *MciSaveConvertTargetName(MciSaveConvertTarget target)
{
    switch (target) {
        case MCI_CONVERT_PS2_PSU: return ".psu  EMS / uLaunchELF / OPL";
        case MCI_CONVERT_PS2_PSV: return ".psv  PS3 (signed)";
        case MCI_CONVERT_PS1_MCS: return ".mcs  MemcardRex / PSXGameEdit";
        case MCI_CONVERT_PS1_PSV: return ".psv  PS3 (signed)";
        case MCI_CONVERT_PS1_PSX: return ".psx  Action Replay / GameShark";
        case MCI_CONVERT_CARD_MCD: return ".mcd  raw card (POPStarter, emulators)";
        case MCI_CONVERT_CARD_GME: return ".gme  DexDrive";
        case MCI_CONVERT_CARD_VGS: return ".vgs  VGS / bleem!";
        case MCI_CONVERT_CARD_VMP: return ".vmp  PSP / Vita (signed)";
        case MCI_CONVERT_CARD_MCX: return ".mcx  MCX (encrypted)";
        default: return "default";
    }
}

int MciSaveConvertTargetsFor(MciSaveTransferFormat source, int generation,
                             MciSaveConvertTarget out[8])
{
    int n = 0;

    if (out == NULL)
        return 0;
    if (generation == 2) {
        out[n++] = MCI_CONVERT_PS2_PSU;
        out[n++] = MCI_CONVERT_PS2_PSV;
    } else if (source == MCI_SAVE_FORMAT_IMAGE_PS1 || source == MCI_SAVE_FORMAT_GME_PS1) {
        out[n++] = MCI_CONVERT_CARD_MCD;
        out[n++] = MCI_CONVERT_CARD_GME;
        out[n++] = MCI_CONVERT_CARD_VGS;
        out[n++] = MCI_CONVERT_CARD_VMP;
        out[n++] = MCI_CONVERT_CARD_MCX;
    } else if (generation == 1) {
        out[n++] = MCI_CONVERT_PS1_MCS;
        out[n++] = MCI_CONVERT_PS1_PSV;
        out[n++] = MCI_CONVERT_PS1_PSX;
    }
    return n;
}
