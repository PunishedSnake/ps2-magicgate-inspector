#!/usr/bin/env python3
"""Fail-closed source invariant checks for the integrated FMCB cross-region path."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

install_c = (ROOT / "src/fmcb_install.c").read_text(encoding="utf-8")
install_h = (ROOT / "src/fmcb_install.h").read_text(encoding="utf-8")
tx_c = (ROOT / "src/fmcb_transaction.c").read_text(encoding="utf-8")
tx_h = (ROOT / "src/fmcb_transaction.h").read_text(encoding="utf-8")
recovery_c = (ROOT / "src/fmcb_recovery.c").read_text(encoding="utf-8")
recovery_h = (ROOT / "src/fmcb_recovery.h").read_text(encoding="utf-8")
marker_c = (ROOT / "src/fmcb_recovery_marker.c").read_text(encoding="utf-8")
diag_c = (ROOT / "src/diag_wrap.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
app_c = (ROOT / "src/app_main.c").read_text(encoding="utf-8")

manifest_match = re.search(
    r"static const FmcbPackageEntry CrossRegionInstallManifest\[\] = \{(.*?)\n\};",
    install_c,
    re.S,
)
assert manifest_match, "CrossRegionInstallManifest not found"

entries = re.findall(
    r'\{"([^"]+)",\s*"([^"]+)",\s*([^}]+)\}',
    manifest_match.group(1),
)
assert len(entries) == 28, f"expected 28 cross-region manifest entries, got {len(entries)}"

destinations = [dest for _, dest, _ in entries]
assert len(destinations) == len(set(destinations)), "duplicate destination in manifest"

expected_boot = {
    "BIEXEC-SYSTEM/osd130.elf",
    "BIEXEC-SYSTEM/osdmain.elf",
    "BEEXEC-SYSTEM/osd130.elf",
    "BEEXEC-SYSTEM/osdmain.elf",
    "BAEXEC-SYSTEM/osd120.elf",
    "BAEXEC-SYSTEM/osd130.elf",
    "BAEXEC-SYSTEM/osdmain.elf",
    "BCEXEC-SYSTEM/osdmain.elf",
    "BIEXEC-SYSTEM/osdsys.elf",
    "BIEXEC-SYSTEM/osd110.elf",
}
missing = expected_boot.difference(destinations)
assert not missing, f"missing cross-region boot aliases: {sorted(missing)}"

for region in ("BI", "BE", "BA", "BC"):
    assert f"{region}EXEC-SYSTEM/FMCB.icn" in destinations
    assert f"{region}EXEC-SYSTEM/icon.sys" in destinations

for path in (
    "BIEXEC-SYSTEM/dev9.irx",
    "BIEXEC-SYSTEM/atad.irx",
    "BIEXEC-SYSTEM/hddload.irx",
    "SYS-CONF/endvdpl.irx",
    "SYS-CONF/FMCB_CFG.ELF",
    "SYS-CONF/FREEMCB.CNF",
    "SYS-CONF/icon.sys",
    "SYS-CONF/sysconf.icn",
    "SYS-CONF/USBD.IRX",
    "SYS-CONF/USBHDFSD.IRX",
):
    assert path in destinations, f"missing destination {path}"

max_entries = re.search(r"#define\s+FMCB_MAX_PACKAGE_ENTRIES\s+(\d+)", install_h)
assert max_entries and int(max_entries.group(1)) >= len(entries)
assert "#define FMCB_CROSS_REGION_SYSTEM_DIRS 4" in install_h
assert 'snprintf(plan->system_dirs[0]' in install_c and '"BIEXEC-SYSTEM"' in install_c
assert 'snprintf(plan->system_dirs[1]' in install_c and '"BEEXEC-SYSTEM"' in install_c
assert 'snprintf(plan->system_dirs[2]' in install_c and '"BAEXEC-SYSTEM"' in install_c
assert 'snprintf(plan->system_dirs[3]' in install_c and '"BCEXEC-SYSTEM"' in install_c

assert "FmcbInstallCrossRegionTransactional" in tx_h
assert "FmcbInstallCrossRegionTransactional" in tx_c
assert "FmcbInstallCrossRegionTransactional" in app_c
assert "FmcbInstallNormalTransactional" not in tx_h
assert "FmcbInstallNormalTransactional" not in tx_c
assert "FmcbInstallNormalTransactional" not in app_c

revalidate = re.search(
    r"static int RevalidateInstallerPreconditions\(.*?\n\}",
    app_c,
    re.S,
)
assert revalidate, "RevalidateInstallerPreconditions not found"
revalidate_text = revalidate.group(0)
assert revalidate_text.index("FmcbProbeMassPackage") < revalidate_text.index("RefreshRecoveryStatus"), (
    "package root must be resolved before recovery revalidation"
)

# Preserve the stronger P0 inventory path while extending directory ownership.
for token in (
    "inventory_exact_rc",
    "inventory_parent_rc",
    "inventory_open_rc",
    "InventoryTargetFromParent",
    "InventoryTargetFromOpen",
):
    assert token in tx_c or token in tx_h, f"P0 inventory invariant missing: {token}"

assert "created_system_dirs[FMCB_CROSS_REGION_SYSTEM_DIRS]" in tx_h
assert "FmcbRecoveryRecordSystemDirectory" in tx_c
assert "FmcbRecoveryRecordSysconfDirectory" in tx_c
assert "FmcbRecoveryRecordDirectories" not in tx_c
assert "#define RECOVERY_VERSION 2u" in recovery_c
assert "created_system_dir_mask" in recovery_c
assert "system_dirs[FMCB_CROSS_REGION_SYSTEM_DIRS][48]" in recovery_c
assert "MciUsbGetVerifiedPackageRoot" in recovery_c
assert "ProbeRecoverySourceRoot" in recovery_c
assert "FmcbRecoveryDiscardEmptyJournal" in recovery_c
assert "TryDiscardUnarmedEmptyJournal" in marker_c
assert "MciUsbGetVerifiedPackageRoot" in marker_c
assert "ReconcileResidualRoot" in marker_c

assert "__wrap_FmcbInstallCrossRegionTransactional" in diag_c
assert "__real_FmcbInstallCrossRegionTransactional" in diag_c
assert "FmcbInstallNormalTransactional" not in diag_c
assert "--wrap=FmcbInstallCrossRegionTransactional" in makefile
assert "--wrap=FmcbInstallNormalTransactional" not in makefile

# Recovery path must remain at least as large as the package-root producer.
assert "FMCB_RECOVERY_PATH_MAX (FMCB_SOURCE_ROOT_MAX + 32)" in recovery_h

print(
    "FMCB cross-region invariants: PASS "
    f"({len(entries)} manifest entries, 10 boot aliases, P0 inventory preserved)"
)
