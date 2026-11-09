#ifndef MCI_SAVE_TRANSFER_H
#define MCI_SAVE_TRANSFER_H

#include <tamtypes.h>

#define MCI_SAVE_TRANSFER_PATH_MAX 256
#define MCI_SAVE_TRANSFER_TITLE_MAX 96
#define MCI_SAVE_TRANSFER_FAILED_PATH_MAX 192

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
    MCI_SAVE_FORMAT_PSV_PS1,

    /* 0.4.2: whole PS1 card images (.mcd/.mcr/.mc/.ddf/.mem/.vgs/.vmp/...)
     * and headerless raw PS1 single saves. GME stays GME_PS1 above. */
    MCI_SAVE_FORMAT_IMAGE_PS1,
    MCI_SAVE_FORMAT_RAW_PS1
} MciSaveTransferFormat;

typedef enum MciSaveTransferFamily {
    MCI_SAVE_FAMILY_UNKNOWN = 0,
    MCI_SAVE_FAMILY_FULL_PS2_IMAGE,
    MCI_SAVE_FAMILY_PS2_SAVE,
    MCI_SAVE_FAMILY_PS1_SAVE
} MciSaveTransferFamily;

typedef enum MciSaveTransferResult {
    MCI_SAVE_TRANSFER_NOT_RUN = 0,
    MCI_SAVE_TRANSFER_OK,
    MCI_SAVE_TRANSFER_INVALID_CONTAINER,
    MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT,
    MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE,
    MCI_SAVE_TRANSFER_TARGET_CONFLICT,
    MCI_SAVE_TRANSFER_TARGET_FULL,
    MCI_SAVE_TRANSFER_IO_ERROR,
    MCI_SAVE_TRANSFER_VERIFY_ERROR,
    MCI_SAVE_TRANSFER_ROLLBACK_FAILED
} MciSaveTransferResult;

typedef struct MciSaveTransferProbe {
    MciSaveTransferFormat format;
    MciSaveTransferFamily family;
    u64 size;
    int readable;
    int format_confidence; /* 0 extension only, 1 signature/structure confirmed */
    int generation;        /* 1 = PS1, 2 = PS2, 0 = unknown */
    int importable;        /* decoder exists and reaches the import contract */
    char path[MCI_SAVE_TRANSFER_PATH_MAX];
} MciSaveTransferProbe;

typedef struct MciSaveTransferReport {
    MciSaveTransferResult result;
    MciSaveTransferFormat format;
    int card_port;
    int files_total;
    int files_written;
    int files_verified;
    int target_free_clusters;
    int required_clusters;
    int rollback_rc;
    u32 bytes_written;
    char source_path[MCI_SAVE_TRANSFER_PATH_MAX];
    char destination[MCI_SAVE_TRANSFER_PATH_MAX];
    char save_directory[33];
    char failed_path[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    /* 0.4.2 */
    int generation;              /* 1 = PS1 card target, 2 = PS2 */
    int decode_rc;               /* MCI_SAVE_E_* from the container decoder */
    int checksum_ok;             /* -1 n/a, 0 bad, 1 good (MAX CRC32) */
    int saves_total;             /* PS1 card images carry several saves */
    int saves_written;
    char title[MCI_SAVE_TRANSFER_TITLE_MAX];
} MciSaveTransferReport;

const char *MciSaveTransferFormatName(MciSaveTransferFormat format);
MciSaveTransferFamily MciSaveTransferFormatFamily(MciSaveTransferFormat format);
MciSaveTransferFormat MciSaveTransferFormatFromPath(const char *path);
int MciSaveTransferProbeFile(const char *path, MciSaveTransferProbe *probe);
const char *MciSaveTransferResultText(MciSaveTransferResult result);
void MciSaveTransferResetReport(MciSaveTransferReport *report, int card_port,
                                MciSaveTransferFormat format);

/* First fully implemented single-save interchange path. Import is fail-closed:
 * no existing save is overwritten, every created file is reopened and compared
 * byte-for-byte with the PSU source, and partial objects are rolled back. */
int MciSaveTransferImportPsu(int target_port, const char *path,
                             MciSaveTransferReport *report);
int MciSaveTransferExportPsu(int source_port, const char *save_directory,
                             const char *path, MciSaveTransferReport *report);

/* Unified dispatcher. Detects the container from its content (extension is
 * only a hint), decodes it fully in RAM, then writes it with the same
 * fail-closed contract as PSU: conflicts refused, every file re-read and
 * compared, partial objects rolled back. PS1 containers require a PS1 card. */
int MciSaveTransferImportFile(int target_port, const char *path,
                              MciSaveTransferReport *report);

/* Content-based detection on a file prefix (>= 4096 bytes or the whole
 * file) plus its real size. Returns UNKNOWN when nothing matches. */
MciSaveTransferFormat MciSaveTransferDetectBuffer(const unsigned char *prefix,
                                                  u32 prefix_size, u64 file_size,
                                                  const char *path,
                                                  int *generation);
int MciSaveTransferFormatImportable(MciSaveTransferFormat format);

/* USB-side conversion (never touches a memory card). The output is written
 * next to the source with a new extension and re-read for verification:
 *   any PS2 container      -> .psu
 *   PS1 single save        -> .mcs
 *   PS1 card image (any)   -> .mcd (raw 128 KiB, POPStarter/emulators)
 * `out_path` receives the written path. */
int MciSaveTransferConvertFile(const char *path, char *out_path,
                               unsigned int out_path_size,
                               MciSaveTransferReport *report);

/* Explicit output format. Signed (PSV, VMP) and encrypted (MCX) outputs are
 * produced with the public container keys and re-verified after writing. */
typedef enum MciSaveConvertTarget {
    MCI_CONVERT_DEFAULT = 0,
    MCI_CONVERT_PS2_PSU,
    MCI_CONVERT_PS2_PSV,
    MCI_CONVERT_PS1_MCS,
    MCI_CONVERT_PS1_PSV,
    MCI_CONVERT_PS1_PSX,
    MCI_CONVERT_CARD_MCD,
    MCI_CONVERT_CARD_GME,
    MCI_CONVERT_CARD_VGS,
    MCI_CONVERT_CARD_VMP,
    MCI_CONVERT_CARD_MCX
} MciSaveConvertTarget;

/* PS1 headerless saves carry no name. The UI sets the user-confirmed name
 * here before the next import/convert; it applies to one PS1 single-save
 * operation and is then cleared. NULL clears it. */
int MciSaveTransferSetPs1NameOverride(const char *name);

const char *MciSaveConvertTargetName(MciSaveConvertTarget target);
/* Targets valid for a source generation/format, in menu order. Returns
 * the count written to out (max 8). */
int MciSaveConvertTargetsFor(MciSaveTransferFormat source, int generation,
                             MciSaveConvertTarget out[8]);
int MciSaveTransferConvertFileTo(const char *path, MciSaveConvertTarget target,
                                 char *out_path, unsigned int out_path_size,
                                 MciSaveTransferReport *report);

/* Default single-save export policy. PSU is uncompressed, metadata-preserving,
 * widely supported and does not depend on proprietary encryption/compression. */
MciSaveTransferFormat MciSaveTransferDefaultPs2ExportFormat(void);

#endif /* MCI_SAVE_TRANSFER_H */
