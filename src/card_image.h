#ifndef MCI_CARD_IMAGE_H
#define MCI_CARD_IMAGE_H

#include <tamtypes.h>

/* Interoperable image formats used by Drebin's hardware-facing Card Tools. */
#define MCI_CARD_IMAGE_PATH_MAX 256

typedef enum MciCardImageFormat {
    MCI_CARD_IMAGE_PS2 = 0,
    MCI_CARD_IMAGE_VMC
} MciCardImageFormat;

typedef enum MciCardImageResult {
    MCI_CARD_IMAGE_NOT_RUN = 0,
    MCI_CARD_IMAGE_OK,
    MCI_CARD_IMAGE_NO_CARD,
    MCI_CARD_IMAGE_GEOMETRY_ERROR,
    MCI_CARD_IMAGE_USB_ERROR,
    MCI_CARD_IMAGE_READ_ERROR,
    MCI_CARD_IMAGE_WRITE_ERROR,
    MCI_CARD_IMAGE_VERIFY_ERROR,
    MCI_CARD_IMAGE_SIZE_MISMATCH,
    MCI_CARD_IMAGE_FORMAT_ERROR
} MciCardImageResult;

typedef struct MciCardGeometry {
    u32 page_size;
    u32 pages_per_cluster;
    u32 pages_per_block;
    u32 total_pages;
    u32 clusters_per_card;
    int from_superblock;
} MciCardGeometry;

typedef struct MciCardImageReport {
    int port;
    MciCardImageFormat format;
    MciCardImageResult result;
    MciCardGeometry geometry;
    u32 logical_crc32;
    u32 pages_done;
    u32 pages_total;
    u64 output_bytes;
    int verify_rc;
    int format_rc;
    int verified;
    char path[MCI_CARD_IMAGE_PATH_MAX];
} MciCardImageReport;

void MciCardImageResetReport(MciCardImageReport *report, int port,
                             MciCardImageFormat format);
const char *MciCardImageFormatName(MciCardImageFormat format);
const char *MciCardImageResultText(MciCardImageResult result);
int MciCardImageProbeGeometry(int port, MciCardGeometry *geometry);
int MciCardImageExport(int port, MciCardImageFormat format,
                       MciCardImageReport *report);
int MciCardImageVerifyFile(const char *path, MciCardImageFormat format,
                           MciCardImageReport *report);
int MciCardImageFindLatest(int port, MciCardImageFormat format,
                           char *path, unsigned int path_size);
int MciCardImageRestoreExact(int port, const char *path,
                             MciCardImageFormat format,
                             MciCardImageReport *report);
int MciCardForceFormatWithBackup(int port, MciCardImageReport *report);

#endif /* MCI_CARD_IMAGE_H */
