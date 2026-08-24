#ifndef MCI_SAVE_TRANSFER_H
#define MCI_SAVE_TRANSFER_H

#include <tamtypes.h>

#define MCI_SAVE_TRANSFER_PATH_MAX 256
#define MCI_SAVE_TRANSFER_TITLE_MAX 96

/*
 * Unified container vocabulary for Card Tools.
 *
 * PS2 and PS1 transfer formats deliberately share one picker/API, but they are
 * not interchangeable. The importer must validate the inserted card type before
 * any write and reject a container whose console generation does not match.
 */
typedef enum MciSaveTransferFormat {
    MCI_SAVE_FORMAT_UNKNOWN = 0,

    /* Full PS2 card images. These are sources for Image Browser/Exact Restore,
     * not single-save archives. */
    MCI_SAVE_FORMAT_IMAGE_PS2,
    MCI_SAVE_FORMAT_IMAGE_VMC,

    /* Single PS2 save containers. */
    MCI_SAVE_FORMAT_PSU,       /* EMS/uLaunchELF, preferred lossless export */
    MCI_SAVE_FORMAT_MAX,       /* Action Replay MAX / MAX Drive */
    MCI_SAVE_FORMAT_PWS,       /* MAX Drive variant */
    MCI_SAVE_FORMAT_CBS,       /* CodeBreaker */
    MCI_SAVE_FORMAT_SPS,       /* SharkPort */
    MCI_SAVE_FORMAT_XPS,       /* X-Port / Xploder */
    MCI_SAVE_FORMAT_PSV_PS2,   /* PS3 exported PS2 save */

    /* PS1 containers belong in the same picker, but only when the destination
     * is a PS1 card. MCS and DexDrive/GME must never be written to a PS2 card. */
    MCI_SAVE_FORMAT_MCS_PS1,
    MCI_SAVE_FORMAT_GME_PS1,
    MCI_SAVE_FORMAT_PSX_PS1,
    MCI_SAVE_FORMAT_PSV_PS1
} MciSaveTransferFormat;

typedef enum MciSaveTransferFamily {
    MCI_SAVE_FAMILY_UNKNOWN = 0,
    MCI_SAVE_FAMILY_FULL_PS2_IMAGE,
    MCI_SAVE_FAMILY_PS2_SAVE,
    MCI_SAVE_FAMILY_PS1_SAVE
} MciSaveTransferFamily;

typedef struct MciSaveTransferProbe {
    MciSaveTransferFormat format;
    MciSaveTransferFamily family;
    u64 size;
    int readable;
    int format_confidence; /* 0 extension only, 1 signature/structure confirmed */
    char path[MCI_SAVE_TRANSFER_PATH_MAX];
} MciSaveTransferProbe;

const char *MciSaveTransferFormatName(MciSaveTransferFormat format);
MciSaveTransferFamily MciSaveTransferFormatFamily(MciSaveTransferFormat format);
MciSaveTransferFormat MciSaveTransferFormatFromPath(const char *path);
int MciSaveTransferProbeFile(const char *path, MciSaveTransferProbe *probe);

/* Default single-save export policy. PSU is uncompressed, metadata-preserving,
 * widely supported and does not depend on proprietary encryption/compression. */
MciSaveTransferFormat MciSaveTransferDefaultPs2ExportFormat(void);

#endif /* MCI_SAVE_TRANSFER_H */
