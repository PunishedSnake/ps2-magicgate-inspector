#!/usr/bin/env python3
"""Reconcile the qualified async-read close boundary with the later direct-slot tree.

This is intentionally tiny and fail-closed.  The bulk of image_read_ahead is
copied bit-for-bit from the qualified eca8ec2 experiment.  The later c88f507
line owns image_write_behind.c because it also contains the producer-facing
Reserve/Commit API.  Only the shared fileXio close boundary needs to be bridged:

* compile the read-ahead drain dependency only for async-read candidates;
* drain an outstanding speculative read before closing any file descriptor;
* restore FXIO_WAIT before the real close;
* preserve the existing write-drain error behavior and direct-slot functions.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one source anchor, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    args = parser.parse_args()

    text = args.source.read_text()

    old_include = '''#include "diag_log.h"\n#include "image_write_behind.h"\n#include "r5900_memops.h"\n\n#ifndef MCI_IMAGE_WRITE_PAGES\n'''
    new_include = '''#include "diag_log.h"\n#include "image_write_behind.h"\n#include "r5900_memops.h"\n\n#ifndef MCI_IMAGE_READ_AHEAD_ASYNC\n#define MCI_IMAGE_READ_AHEAD_ASYNC 0\n#endif\n\n#if MCI_IMAGE_READ_AHEAD_ASYNC\n#include "image_read_ahead.h"\n#endif\n\n#ifndef MCI_IMAGE_WRITE_PAGES\n'''
    text = replace_once(text, old_include, new_include, "read-drain include bridge")

    old_close = '''int __wrap_fileXioClose(int fd)\n{\n    int drain_rc = 0;\n    int close_rc;\n\n    if (EnableDepth != 0u) {\n        drain_rc = DrainAsync();\n        if (drain_rc < 0 && LastError == 0)\n            LastError = drain_rc;\n\n        if (fd == ActiveFd) {\n            /* Successful full-card images are exact multiples of every allowed\n             * batch size. Anything left here belongs to an aborted operation and\n             * must not be pushed after the producer already reported failure. */\n            ResetSlot(FillSlot);\n            ActiveFd = -1;\n            ActiveStride = 0;\n        }\n    }\n\n    fileXioSetBlockMode(FXIO_WAIT);\n    close_rc = __real_fileXioClose(fd);\n    if (drain_rc < 0 && close_rc >= 0)\n        return drain_rc;\n    return close_rc;\n}\n'''

    new_close = '''int __wrap_fileXioClose(int fd)\n{\n    int drain_rc = 0;\n    int read_drain_rc = 0;\n    int close_rc;\n\n#if MCI_IMAGE_READ_AHEAD_ASYNC\n    /* CURRENT IMPLEMENTATION: fileXio exposes one global NOWAIT completion\n     * state. A speculative read must relinquish it before any fd is closed. */\n    read_drain_rc = MciImageReadAheadDrain();\n#endif\n\n    if (EnableDepth != 0u) {\n        drain_rc = DrainAsync();\n        if (drain_rc < 0 && LastError == 0)\n            LastError = drain_rc;\n\n        if (fd == ActiveFd) {\n            /* Successful full-card images are exact multiples of every allowed\n             * batch size. Anything left here belongs to an aborted operation and\n             * must not be pushed after the producer already reported failure. */\n            ResetSlot(FillSlot);\n            ActiveFd = -1;\n            ActiveStride = 0;\n        }\n    }\n\n    fileXioSetBlockMode(FXIO_WAIT);\n    close_rc = __real_fileXioClose(fd);\n    if (read_drain_rc < 0 && close_rc >= 0)\n        return read_drain_rc;\n    if (drain_rc < 0 && close_rc >= 0)\n        return drain_rc;\n    return close_rc;\n}\n'''
    text = replace_once(text, old_close, new_close, "fileXio close ownership bridge")

    args.source.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
