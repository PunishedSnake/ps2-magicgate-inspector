/* SPDX-License-Identifier: MIT */
/* Unified Card Tools file/container probing. */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "save_transfer.h"

static int EndsWithIcase(const char *text, const char *suffix)
{
    size_t tl;
    size_t sl;
    size_t i;

    if (text == NULL || suffix == NULL)
        return 0;
    tl = strlen(text);
    sl = strlen(suffix);
    if (tl < sl)
        return 0;
    text += tl - sl;
    for (i = 0; i < sl; i++) {
        if (tolower((unsigned char)text[i]) !=
            tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

MciSaveTransferFamily MciSaveTransferFormatFamily(MciSaveTransferFormat format)
{
    switch (format) {
        case MCI_SAVE_FORMAT_IMAGE_PS2:
        case MCI_SAVE_FORMAT_IMAGE_VMC:
            return MCI_SAVE_FAMILY_FULL_PS2_IMAGE;
        case MCI_SAVE_FORMAT_PSU:
        case MCI_SAVE_FORMAT_MAX:
        case MCI_SAVE_FORMAT_PWS:
        case MCI_SAVE_FORMAT_CBS:
        case MCI_SAVE_FORMAT_SPS:
        case MCI_SAVE_FORMAT_XPS:
        case MCI_SAVE_FORMAT_PSV_PS2:
            return MCI_SAVE_FAMILY_PS2_SAVE;
        case MCI_SAVE_FORMAT_MCS_PS1:
        case MCI_SAVE_FORMAT_GME_PS1:
        case MCI_SAVE_FORMAT_PSX_PS1:
        case MCI_SAVE_FORMAT_PSV_PS1:
            return MCI_SAVE_FAMILY_PS1_SAVE;
        default:
            return MCI_SAVE_FAMILY_UNKNOWN;
    }
}

const char *MciSaveTransferFormatName(MciSaveTransferFormat format)
{
    switch (format) {
        case MCI_SAVE_FORMAT_IMAGE_PS2: return "PCSX2 .ps2 image";
        case MCI_SAVE_FORMAT_IMAGE_VMC: return "OPL .vmc image";
        case MCI_SAVE_FORMAT_PSU: return "PS2 PSU (EMS/uLaunchELF)";
        case MCI_SAVE_FORMAT_MAX: return "PS2 MAX Drive";
        case MCI_SAVE_FORMAT_PWS: return "PS2 MAX Drive .pws";
        case MCI_SAVE_FORMAT_CBS: return "PS2 CodeBreaker .cbs";
        case MCI_SAVE_FORMAT_SPS: return "PS2 SharkPort .sps";
        case MCI_SAVE_FORMAT_XPS: return "PS2 X-Port .xps";
        case MCI_SAVE_FORMAT_PSV_PS2: return "PS3 PS2 save .psv";
        case MCI_SAVE_FORMAT_MCS_PS1: return "PS1 MCS .mcs";
        case MCI_SAVE_FORMAT_GME_PS1: return "PS1 DexDrive .gme";
        case MCI_SAVE_FORMAT_PSX_PS1: return "PS1 AR/GS/XP .psx";
        case MCI_SAVE_FORMAT_PSV_PS1: return "PS3 PS1 save .psv";
        default: return "Unknown";
    }
}

MciSaveTransferFormat MciSaveTransferFormatFromPath(const char *path)
{
    if (path == NULL)
        return MCI_SAVE_FORMAT_UNKNOWN;
    if (EndsWithIcase(path, ".ps2")) return MCI_SAVE_FORMAT_IMAGE_PS2;
    if (EndsWithIcase(path, ".vmc")) return MCI_SAVE_FORMAT_IMAGE_VMC;
    if (EndsWithIcase(path, ".psu")) return MCI_SAVE_FORMAT_PSU;
    if (EndsWithIcase(path, ".max")) return MCI_SAVE_FORMAT_MAX;
    if (EndsWithIcase(path, ".pws")) return MCI_SAVE_FORMAT_PWS;
    if (EndsWithIcase(path, ".cbs")) return MCI_SAVE_FORMAT_CBS;
    if (EndsWithIcase(path, ".sps")) return MCI_SAVE_FORMAT_SPS;
    if (EndsWithIcase(path, ".xps")) return MCI_SAVE_FORMAT_XPS;
    if (EndsWithIcase(path, ".mcs")) return MCI_SAVE_FORMAT_MCS_PS1;
    if (EndsWithIcase(path, ".gme")) return MCI_SAVE_FORMAT_GME_PS1;
    if (EndsWithIcase(path, ".psx")) return MCI_SAVE_FORMAT_PSX_PS1;
    /* PSV contains a generation discriminator in its header. Until the PSV
     * parser validates that field, keep extension-only PSV deliberately
     * ambiguous instead of guessing PS1 vs PS2. */
    if (EndsWithIcase(path, ".psv")) return MCI_SAVE_FORMAT_UNKNOWN;
    return MCI_SAVE_FORMAT_UNKNOWN;
}

static int ContainsBytes(const unsigned char *haystack, unsigned int haystack_size,
                         const char *needle, unsigned int needle_size)
{
    unsigned int i;

    if (haystack == NULL || needle == NULL || needle_size == 0u ||
        haystack_size < needle_size)
        return 0;
    for (i = 0u; i + needle_size <= haystack_size; i++) {
        if (memcmp(haystack + i, needle, needle_size) == 0)
            return 1;
    }
    return 0;
}

int MciSaveTransferProbeFile(const char *path, MciSaveTransferProbe *probe)
{
    unsigned char head[96];
    iox_stat_t stat;
    MciSaveTransferFormat by_ext;
    int fd;
    int read_rc;

    if (path == NULL || probe == NULL)
        return -1;
    memset(probe, 0, sizeof(*probe));
    probe->format = MCI_SAVE_FORMAT_UNKNOWN;
    probe->family = MCI_SAVE_FAMILY_UNKNOWN;
    snprintf(probe->path, sizeof(probe->path), "%s", path);

    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) < 0 || !FIO_S_ISREG(stat.mode) || stat.size <= 0)
        return -2;
    probe->size = (u64)stat.size;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    memset(head, 0, sizeof(head));
    read_rc = fileXioRead(fd, head, sizeof(head));
    fileXioClose(fd);
    if (read_rc < 0)
        return read_rc;
    probe->readable = 1;

    by_ext = MciSaveTransferFormatFromPath(path);
    probe->format = by_ext;

    /* Confirm the formats with stable public signatures. Importers still do
     * complete structural validation before creating anything on a card. */
    if (read_rc >= 12 && memcmp(head, "Ps2PowerSave", 12) == 0) {
        probe->format = EndsWithIcase(path, ".pws")
                            ? MCI_SAVE_FORMAT_PWS : MCI_SAVE_FORMAT_MAX;
        probe->format_confidence = 1;
    } else if (read_rc >= 4 && memcmp(head, "CFU\0", 4) == 0) {
        probe->format = MCI_SAVE_FORMAT_CBS;
        probe->format_confidence = 1;
    } else if (ContainsBytes(head, (unsigned int)read_rc,
                             "SharkPortSave", 13u)) {
        probe->format = EndsWithIcase(path, ".sps")
                            ? MCI_SAVE_FORMAT_SPS : MCI_SAVE_FORMAT_XPS;
        probe->format_confidence = 1;
    } else if (by_ext != MCI_SAVE_FORMAT_UNKNOWN) {
        probe->format_confidence = 0;
    }

    probe->family = MciSaveTransferFormatFamily(probe->format);
    return 0;
}

MciSaveTransferFormat MciSaveTransferDefaultPs2ExportFormat(void)
{
    return MCI_SAVE_FORMAT_PSU;
}
