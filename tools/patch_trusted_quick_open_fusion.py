#!/usr/bin/env python3
"""Fuse trusted quick verification with OpenImage's long-lived source fd.

This transformer is intentionally applied *after* the FINAL-SAFE-v2 synchronous
transforms, including trusted quick-verify read-ahead elision. It removes the
second open/lseek/page-zero read/close sequence while preserving the existing
preverify fileXioSync, quick size/geometry validation, filesystem superblock
validation, logger pause depth, and full-scan behavior.

Every edit is fail-closed: unexpected source drift aborts the transform.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one source match, found {count}")
    return text.replace(old, new, 1)


def replace_function(text: str, signature: str, replacement: str, label: str) -> str:
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"{label}: signature not found")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"{label}: opening brace not found")
    depth = 0
    end = None
    for pos in range(brace, len(text)):
        ch = text[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                break
    if end is None:
        raise SystemExit(f"{label}: closing brace not found")
    return text[:start] + replacement.rstrip() + text[end:]


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "/* Revalidate image size and page-zero filesystem geometry without a second\n"
        " * complete sequential pass. Used only after Image Browser already completed a\n"
        " * full verification in the same selection workflow. */\n"
        "int MciCardImageQuickReopenVerify(const char *path,\n",
        "/* Validate size and page-zero geometry and transfer ownership of the open\n"
        " * source descriptor to the caller on success. `first` receives the already\n"
        " * consumed page zero, `fd_out` receives the owned fd. On every error this\n"
        " * function closes any descriptor it opened and leaves *fd_out == -1. */\n"
        "int MciCardImageQuickOpenVerify(const char *path,\n"
        "                                MciCardImageFormat format,\n"
        "                                MciCardImageReport *report,\n"
        "                                unsigned char first[512],\n"
        "                                int *fd_out);\n\n"
        "/* Revalidate image size and page-zero filesystem geometry without a second\n"
        " * complete sequential pass. Compatibility API: it consumes the ownership\n"
        " * returned by MciCardImageQuickOpenVerify and closes the descriptor. */\n"
        "int MciCardImageQuickReopenVerify(const char *path,\n",
        "quick-open declaration",
    )
    path.write_text(text, encoding="utf-8")


def patch_quick_verify(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    replacement = r'''int MciCardImageQuickOpenVerify(const char *path,
                                MciCardImageFormat format,
                                MciCardImageReport *report,
                                unsigned char first[QV_PAGE_DATA],
                                int *fd_out)
{
    iox_stat_t stat;
    MciCardGeometry geometry;
    u32 stride = format == MCI_CARD_IMAGE_PS2 ? QV_PAGE_RAW : QV_PAGE_DATA;
    u32 pages;
    int fd = -1;
    int rc;

    if (path == NULL || report == NULL || first == NULL || fd_out == NULL)
        return -1;
    *fd_out = -1;
    MciCardImageResetReport(report, -1, format);
    snprintf(report->path, sizeof(report->path), "%s", path);

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0 || stat.size == 0u || (stat.size % stride) != 0u)
        goto invalid;
    pages = stat.size / stride;
    if (!KnownPageCount(pages))
        goto invalid;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        report->verify_rc = fd;
        return fd;
    }
    rc = ReadExact(fd, first, QV_PAGE_DATA);
    if (rc < 0)
        goto invalid_close;
    if (ParseGeometry(first, &geometry) < 0 || geometry.total_pages != pages)
        goto invalid_close;

    report->geometry = geometry;
    report->pages_total = pages;
    report->pages_done = 1u;
    report->output_bytes = stat.size;
    report->verified = 1;
    report->verify_rc = 0;
    report->result = MCI_CARD_IMAGE_OK;
    *fd_out = fd;
    return 0;

invalid_close:
    fileXioClose(fd);
invalid:
    report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
    report->verify_rc = -40;
    return -40;
}

int MciCardImageQuickReopenVerify(const char *path,
                                  MciCardImageFormat format,
                                  MciCardImageReport *report)
{
    unsigned char first[QV_PAGE_DATA] __attribute__((aligned(64)));
    int fd = -1;
    int rc = MciCardImageQuickOpenVerify(path, format, report, first, &fd);

    if (fd >= 0)
        fileXioClose(fd);
    return rc;
}
'''
    text = replace_function(
        text,
        "int MciCardImageQuickReopenVerify(const char *path,",
        replacement,
        "MciCardImageQuickReopenVerify",
    )
    path.write_text(text, encoding="utf-8")


def patch_card_image_fs(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#include "card_image_fs.h"\n#include "progress.h"\n#include "save_title.h"\n',
        '#include "card_image_fs.h"\n#include "image_quick_verify.h"\n#include "progress.h"\n#include "save_title.h"\n',
        "card_image_fs include",
    )

    replacement = r'''static int OpenImage(const char *path, MciCardImageFormat format,
                     MciFsImage *image, MciCardGeometry *geometry,
                     int trusted_reopen)
{
    MciCardImageReport verify;
    unsigned char first[FS_PAGE_SIZE] __attribute__((aligned(64)));
    int owned_fd = -1;
    int rc;

    memset(image, 0, sizeof(*image));
    image->fd = -1;

    if (trusted_reopen) {
        /* Image Browser already completed the full source pass. Revalidate the
         * same trusted contract (size + page-zero geometry) but transfer the
         * verified descriptor and page-zero bytes directly into OpenImage.
         * Ownership becomes MciFsImage::fd only after the helper succeeds. */
        rc = MciCardImageQuickOpenVerify(path, format, &verify, first, &owned_fd);
        if (rc < 0)
            return rc;
    } else {
        rc = MciCardImageVerifyFile(path, format, &verify);
        if (rc < 0)
            return rc;
    }

    image->format = format;
    image->stride = format == MCI_CARD_IMAGE_PS2 ? FS_PS2_STRIDE : FS_PAGE_SIZE;

    if (trusted_reopen) {
        image->fd = owned_fd;
    } else {
        image->fd = fileXioOpen(path, FIO_O_RDONLY);
        if (image->fd < 0)
            return image->fd;

        /* Full-scan path has not consumed page zero from this descriptor. */
        rc = fileXioLseek(image->fd, 0, SEEK_SET);
        if (rc < 0 || ReadExact(image->fd, first, sizeof(first)) < 0) {
            fileXioClose(image->fd);
            image->fd = -1;
            return -2;
        }
    }

    memcpy(&image->sb, first, sizeof(image->sb));
    /* pages_per_block is physical erase geometry, not a filesystem-browser
     * constraint. The browser never erases the source image. */
    if (memcmp(image->sb.magic, SuperblockMagic, sizeof(SuperblockMagic)) != 0 ||
        image->sb.page_len != FS_PAGE_SIZE ||
        (image->sb.pages_per_cluster != 1u && image->sb.pages_per_cluster != 2u) ||
        image->sb.pages_per_block == 0u ||
        image->sb.clusters_per_card == 0u ||
        image->sb.alloc_offset >= image->sb.clusters_per_card ||
        image->sb.alloc_end == 0u ||
        image->sb.alloc_offset + image->sb.alloc_end > image->sb.clusters_per_card ||
        image->sb.rootdir_cluster != 0u) {
        fileXioClose(image->fd);
        image->fd = -1;
        return -3;
    }
    image->cluster_bytes = image->sb.page_len * image->sb.pages_per_cluster;
    if (image->cluster_bytes == 0u || image->cluster_bytes > FS_MAX_CLUSTER_BYTES ||
        (image->cluster_bytes % sizeof(MciFsDirEntry)) != 0u) {
        fileXioClose(image->fd);
        image->fd = -1;
        return -4;
    }
    image->dir_entries_per_cluster = image->cluster_bytes / sizeof(MciFsDirEntry);
    image->fat_entries_per_cluster = image->cluster_bytes / sizeof(u32);
    if (geometry != NULL)
        *geometry = verify.geometry;
    return 0;
}
'''
    text = replace_function(text, "static int OpenImage(const char *path,", replacement, "OpenImage")
    text = replace_once(
        text,
        "    rc = OpenImage(path, format, &image, &list->geometry);\n",
        "    rc = OpenImage(path, format, &image, &list->geometry, 0);\n",
        "scan OpenImage call",
    )
    text = replace_once(
        text,
        "    rc = OpenImage(list->path, list->format, &image, NULL);\n",
        "    rc = OpenImage(list->path, list->format, &image, NULL, 1);\n",
        "import OpenImage call",
    )
    path.write_text(text, encoding="utf-8")


def patch_diag_wrap(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "static int TrustedImportVerifyActive;\n"
        "static MciCardImageFormat TrustedImportFormat;\n"
        "static char TrustedImportPath[MCI_CARD_IMAGE_PATH_MAX];\n",
        "",
        "trusted global state removal",
    )

    text = replace_once(
        text,
        "int __real_MciCardImageVerifyFile(const char *path, MciCardImageFormat format,\n"
        "                                  MciCardImageReport *report);\n",
        "int __real_MciCardImageVerifyFile(const char *path, MciCardImageFormat format,\n"
        "                                  MciCardImageReport *report);\n"
        "int __real_MciCardImageQuickOpenVerify(const char *path,\n"
        "                                       MciCardImageFormat format,\n"
        "                                       MciCardImageReport *report,\n"
        "                                       unsigned char first[512],\n"
        "                                       int *fd_out);\n",
        "quick-open real declaration",
    )

    verify_replacement = r'''int __wrap_MciCardImageVerifyFile(const char *path, MciCardImageFormat format,
                                  MciCardImageReport *report)
{
    int rc;
    int sync_rc;

    sync_rc = SyncPathDevice(path);
    MciDiagLogPrintf("IMAGE", "verify begin format=%s path=%s preverify_sync=%d",
                     MciCardImageFormatName(format), path != NULL ? path : "NULL",
                     sync_rc);
    MciDiagLogSetMassWritePaused(1);
    MciImageReadAheadSetEnabled(1);
    rc = __real_MciCardImageVerifyFile(path, format, report);

    if (rc == -4 && format == MCI_CARD_IMAGE_PS2 && path != NULL) {
        unsigned char actual[12];
        unsigned char expected[12];
        u32 page = 0u;
        int find_rc = MciCardImageFindFirstEccMismatch(path, &page,
                                                       actual, expected);
        if (find_rc == 1) {
            MciDiagLogPrintf("IMAGE-ECC",
                             "first mismatch page=%u actual=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X expected=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                             page,
                             actual[0], actual[1], actual[2], actual[3],
                             actual[4], actual[5], actual[6], actual[7],
                             actual[8], actual[9], actual[10], actual[11],
                             expected[0], expected[1], expected[2], expected[3],
                             expected[4], expected[5], expected[6], expected[7],
                             expected[8], expected[9], expected[10], expected[11]);
        } else {
            MciDiagLogPrintf("IMAGE-ECC",
                             "mismatch locator rc=%d after verifier rc=-4",
                             find_rc);
        }
    }

    MciImageReadAheadSetEnabled(0);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport("verify", rc, report);
    return rc;
}

int __wrap_MciCardImageQuickOpenVerify(const char *path,
                                       MciCardImageFormat format,
                                       MciCardImageReport *report,
                                       unsigned char first[512],
                                       int *fd_out)
{
    int rc;
    int sync_rc = SyncPathDevice(path);

    MciDiagLogPrintf("IMAGE",
                     "trusted reopen verify begin format=%s path=%s preverify_sync=%d ownership=quick-open",
                     MciCardImageFormatName(format), path != NULL ? path : "NULL",
                     sync_rc);
    /* The import wrapper already owns one pause level. Keep this local level so
     * direct future callers remain safe too; diag_log is explicitly depth-based. */
    MciDiagLogSetMassWritePaused(1);
    rc = __real_MciCardImageQuickOpenVerify(path, format, report, first, fd_out);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport("trusted reopen verify", rc, report);
    return rc;
}
'''
    text = replace_function(
        text,
        "int __wrap_MciCardImageVerifyFile(const char *path,",
        verify_replacement,
        "MciCardImageVerifyFile wrapper",
    )

    old_setup = (
        "    TrustedImportVerifyActive = 0;\n"
        "    TrustedImportPath[0] = '\\0';\n"
        "    if (list != NULL && list->path[0] != '\\0') {\n"
        "        snprintf(TrustedImportPath, sizeof(TrustedImportPath), \"%s\", list->path);\n"
        "        TrustedImportFormat = list->format;\n"
        "        TrustedImportVerifyActive = 1;\n"
        "        MciDiagLogPrintf(\"IMAGE-FS\",\n"
        "                         \"selected-save import trusts prior full scan; second pass reduced to size/superblock reopen validation\");\n"
        "    }\n"
    )
    new_setup = (
        "    if (list != NULL && list->path[0] != '\\0') {\n"
        "        MciDiagLogPrintf(\"IMAGE-FS\",\n"
        "                         \"selected-save import trusts prior full scan; quick validation transfers the verified open source descriptor into the import stream\");\n"
        "    }\n"
    )
    text = replace_once(text, old_setup, new_setup, "trusted import setup")
    text = replace_once(
        text,
        "    MciDiagLogSetMassWritePaused(0);\n"
        "    TrustedImportVerifyActive = 0;\n"
        "    TrustedImportPath[0] = '\\0';\n\n"
        "    MciDiagLogPrintf(\"IMAGE-FS\",\n",
        "    MciDiagLogSetMassWritePaused(0);\n\n"
        "    MciDiagLogPrintf(\"IMAGE-FS\",\n",
        "trusted import teardown",
    )
    path.write_text(text, encoding="utf-8")


def patch_makefile(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "\t-Wl,--wrap=MciCardImageVerifyFile \\\n",
        "\t-Wl,--wrap=MciCardImageVerifyFile \\\n"
        "\t-Wl,--wrap=MciCardImageQuickOpenVerify \\\n",
        "quick-open linker wrap",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root

    patch_header(root / "src/image_quick_verify.h")
    patch_quick_verify(root / "src/image_quick_verify.c")
    patch_card_image_fs(root / "src/card_image_fs.c")
    patch_diag_wrap(root / "src/diag_wrap.c")
    patch_makefile(root / "Makefile")


if __name__ == "__main__":
    main()
