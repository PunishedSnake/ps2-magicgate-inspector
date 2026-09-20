#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"missing patch anchor in {path}: {old!r}")
    p.write_text(text.replace(old, new, 1))

# libmc explicitly documents mode -1 for an erase that will immediately be
# followed by mcWritePage calls. This prevents MCMAN from doing normal erase-time
# ECC work on pages we are about to replace wholesale.
replace_once(
    "src/card_image.c",
    "    mcEraseBlock(port, 0, (int)block, 0);\n",
    "    mcEraseBlock(port, 0, (int)block, -1);\n",
)

# Image verification is usable without a physical card and therefore resets its
# report port to -1 internally. When invoked from Card Tools, restore the UI's
# selected physical slot before presenting the result.
replace_once(
    "src/app_main.c",
    "    if (rc == 0)\n        rc = MciCardImageVerifyFile(path, format, &report);\n    else {\n",
    "    if (rc == 0) {\n        rc = MciCardImageVerifyFile(path, format, &report);\n        report.port = port;\n    } else {\n",
)

# Sanity-check the intended result.
if "mcEraseBlock(port, 0, (int)block, -1);" not in Path("src/card_image.c").read_text():
    raise SystemExit("erase-mode fix did not apply")
if "report.port = port;" not in Path("src/app_main.c").read_text():
    raise SystemExit("verification port fix did not apply")
