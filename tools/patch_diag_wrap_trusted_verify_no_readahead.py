#!/usr/bin/env python3
"""Elide read-ahead ownership for trusted 512-byte quick reopen verification.

The checked-in runtime stays on the hardware-qualified baseline. CI applies this
transform to an isolated worktree. Exact anchors intentionally fail closed if
`diag_wrap.c` drifts.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one source match, found {count}")
    return text.replace(old, new, 1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "    MciDiagLogSetMassWritePaused(1);\n"
        "    MciImageReadAheadSetEnabled(1);\n"
        "    if (trusted)\n"
        "        rc = MciCardImageQuickReopenVerify(path, format, report);\n"
        "    else\n"
        "        rc = __real_MciCardImageVerifyFile(path, format, report);\n",
        "    MciDiagLogSetMassWritePaused(1);\n"
        "    /* Trusted selected-save import already completed a full filesystem\n"
        "     * scan. Its reopen verifier consumes only page zero (512 bytes), so\n"
        "     * do not arm the 16,896-byte sequential read-ahead cache for this\n"
        "     * one-record validation. Full verification keeps the existing scope. */\n"
        "    if (!trusted)\n"
        "        MciImageReadAheadSetEnabled(1);\n"
        "    if (trusted)\n"
        "        rc = MciCardImageQuickReopenVerify(path, format, report);\n"
        "    else\n"
        "        rc = __real_MciCardImageVerifyFile(path, format, report);\n",
        "trusted verify enable",
    )

    text = replace_once(
        text,
        "    MciImageReadAheadSetEnabled(0);\n"
        "    MciDiagLogSetMassWritePaused(0);\n"
        "    LogImageReport(trusted ? \"trusted reopen verify\" : \"verify\", rc, report);\n",
        "    if (!trusted)\n"
        "        MciImageReadAheadSetEnabled(0);\n"
        "    MciDiagLogSetMassWritePaused(0);\n"
        "    LogImageReport(trusted ? \"trusted reopen verify\" : \"verify\", rc, report);\n",
        "trusted verify disable",
    )

    args.source.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
