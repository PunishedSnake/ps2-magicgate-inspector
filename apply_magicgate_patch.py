#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

PATCH_NAME = "PS2 MagicGate Card Inspector & Forced System Update Installer"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def patch_file(path: Path, edits: list[tuple[str, str, str]], dry_run: bool) -> None:
    original = path.read_text(encoding="utf-8")
    modified = original
    for label, old, new in edits:
        modified = replace_once(modified, old, new, f"{path}: {label}")

    if modified == original:
        raise RuntimeError(f"{path}: no changes produced")

    if not dry_run:
        backup = path.with_suffix(path.suffix + ".mgci.bak")
        if not backup.exists():
            shutil.copy2(path, backup)
        path.write_text(modified, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=f"Apply {PATCH_NAME} to israpps/FreeMcBoot-Installer")
    parser.add_argument("repo", type=Path, help="Path to a FreeMcBoot-Installer checkout")
    parser.add_argument("--dry-run", action="store_true", help="Validate anchors without modifying files")
    args = parser.parse_args()

    repo = args.repo.resolve()
    installer = repo / "installer"
    if not installer.is_dir():
        print(f"error: {installer} does not exist", file=sys.stderr)
        return 2

    required = [
        installer / "Makefile",
        installer / "lang.h",
        installer / "lang.c",
        installer / "system.h",
        installer / "system.c",
        installer / "menu.c",
    ]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        print("error: missing required upstream files:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 2

    makefile_edits = [
        (
            "add mg_inspector object",
            "EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o",
            "EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system.o mg_inspector.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o",
        )
    ]

    lang_h_edits = [
        (
            "add inspector label id",
            "    SYS_UI_LBL_FORMAT_HDD,\n\n    SYS_UI_LBL_COUNT",
            "    SYS_UI_LBL_FORMAT_HDD,\n    SYS_UI_LBL_MG_INSPECTOR,\n\n    SYS_UI_LBL_COUNT",
        )
    ]

    lang_c_edits = [
        (
            "add inspector default label",
            '    "Format HDD",\n};',
            '    "Format HDD",\n    "MagicGate Inspector",\n};',
        )
    ]

    system_h_edits = [
        (
            "export validated override",
            "int GetNumMemcardsInserted(struct McData *McData);\n\nint SysCreateThread",
            "int GetNumMemcardsInserted(struct McData *McData);\n\n/* Temporary card-type override. It is only valid for a port/slot that passed MGInspectorRun(). */\nvoid SetMagicGateValidatedOverride(int port, int slot, int enabled);\nint IsMagicGateValidatedOverride(int port, int slot);\n\nint SysCreateThread",
        )
    ]

    system_c_edits = [
        (
            "add validated override state",
            'static char romver[16];\n\nstatic int InitMCInfo(int port, int slot)',
            '''static char romver[16];

/*
 * The normal installer treats MC_TYPE_PS2 as a policy decision. The forced
 * path replaces that one heuristic with a short-lived, per-slot capability
 * result produced by the MagicGate inspector. All real I/O and KELF signing
 * errors remain fatal.
 */
static int MagicGateValidatedPort = -1;
static int MagicGateValidatedSlot = -1;

void SetMagicGateValidatedOverride(int port, int slot, int enabled)
{
    if (enabled) {
        MagicGateValidatedPort = port;
        MagicGateValidatedSlot = slot;
    } else {
        MagicGateValidatedPort = -1;
        MagicGateValidatedSlot = -1;
    }
}

int IsMagicGateValidatedOverride(int port, int slot)
{
    return (MagicGateValidatedPort == port && MagicGateValidatedSlot == slot);
}

static int InitMCInfo(int port, int slot)''',
        ),
        (
            "allow validated card in InitMCInfo",
            "    if (result >= sceMcResChangedCard && type == sceMcTypePS2) {",
            "    if (result >= sceMcResChangedCard && (type == sceMcTypePS2 || IsMagicGateValidatedOverride(port, slot))) {",
        ),
        (
            "allow validated card free-space query",
            "    if (result < -1 || type != MC_TYPE_PS2) {\n        space = 0;\n    }",
            "    if (result < -1 || (type != MC_TYPE_PS2 && !IsMagicGateValidatedOverride(port, slot))) {\n        space = 0;\n    }",
        ),
    ]

    menu_include_old = '#include "UI.h"\n#include "menu.h"\n'
    menu_include_new = '#include "UI.h"\n#include "menu.h"\n#include "mg_inspector.h"\n\n#define EVENT_MG_INSPECT 0xFE\n'

    menu_id_old = '''    MAIN_MENU_ID_BTN_DUMP_MC,
    MAIN_MENU_ID_BTN_REST_MC,
    MAIN_MENU_ID_BTN_INST_CROSS_PSX,'''
    menu_id_new = '''    MAIN_MENU_ID_BTN_DUMP_MC,
    MAIN_MENU_ID_BTN_REST_MC,
    MAIN_MENU_ID_BTN_MG_INSPECT,
    MAIN_MENU_ID_BTN_INST_CROSS_PSX,'''

    mc_menu_old = '''    {MITEM_BUTTON, MAIN_MENU_ID_BTN_REST_MC, MITEM_FLAG_POS_MID, 0, 24, 0, 0, SYS_UI_LBL_REST_MC},
    {MITEM_BREAK},
    {MITEM_BREAK},

    {MITEM_STRING, MAIN_MENU_ID_DESCRIPTION,'''
    mc_menu_new = '''    {MITEM_BUTTON, MAIN_MENU_ID_BTN_REST_MC, MITEM_FLAG_POS_MID, 0, 24, 0, 0, SYS_UI_LBL_REST_MC},
    {MITEM_BREAK},
    {MITEM_BREAK},
    {MITEM_BUTTON, MAIN_MENU_ID_BTN_MG_INSPECT, MITEM_FLAG_POS_MID, 0, 24, 0, 0, SYS_UI_LBL_MG_INSPECTOR},
    {MITEM_BREAK},
    {MITEM_BREAK},

    {MITEM_STRING, MAIN_MENU_ID_DESCRIPTION,'''

    callback_old = '''                case MAIN_MENU_ID_BTN_REST_MC:
                    UISetString(menu, MAIN_MENU_ID_DESCRIPTION, GetUIString(SYS_UI_MSG_DSC_REST_MC));
                    break;
                case MAIN_MENU_ID_BTN_INST_CROSS_PSX:'''
    callback_new = '''                case MAIN_MENU_ID_BTN_REST_MC:
                    UISetString(menu, MAIN_MENU_ID_DESCRIPTION, GetUIString(SYS_UI_MSG_DSC_REST_MC));
                    break;
                case MAIN_MENU_ID_BTN_MG_INSPECT:
                    UISetString(menu, MAIN_MENU_ID_DESCRIPTION, "Test real MagicGate KELF binding and unlock a guarded FMCB force-install path only after a full pass.");
                    break;
                case MAIN_MENU_ID_BTN_INST_CROSS_PSX:'''

    event_map_old = '''            case MAIN_MENU_ID_BTN_REST_MC:
                event = EVENT_RESTORE_MC;
                break;
            case MAIN_MENU_ID_BTN_INST_FHDB:'''
    event_map_new = '''            case MAIN_MENU_ID_BTN_REST_MC:
                event = EVENT_RESTORE_MC;
                break;
            case MAIN_MENU_ID_BTN_MG_INSPECT:
                event = EVENT_MG_INSPECT;
                break;
            case MAIN_MENU_ID_BTN_INST_FHDB:'''

    helper_anchor = '''void MainMenu(void)
{
    int result;'''

    helper_code = r'''static void DisplayValidatedInstallResult(int result)
{
    if (result < 0) {
        switch (-result) {
            case ENOENT:
                DisplayErrorMessage(SYS_UI_MSG_NO_ENT_ERROR);
                break;
            case (EIO | ERROR_SIDE_SRC):
                DisplayErrorMessage(SYS_UI_MSG_READ_INST_ERROR);
                break;
            case (EIO | ERROR_SIDE_DST):
                DisplayErrorMessage(SYS_UI_MSG_WRITE_INST_ERROR);
                break;
            case ENOMEM:
                DisplayErrorMessage(SYS_UI_MSG_NO_MEM_ERROR);
                break;
            case EEXTCACHEINITERR:
                DisplayErrorMessage(SYS_UI_MSG_CACHE_INIT_ERROR);
                break;
            case EEXTCRSLNKFAIL:
                DisplayErrorMessage(SYS_UI_MSG_CROSSLINK_FAIL);
                break;
            case EEXTMGSIGNERR:
                DisplayErrorMessage(SYS_UI_MSG_MG_BIND_FAIL);
                break;
        }

        DisplayErrorMessage(SYS_UI_MSG_INSTALL_FAILED);
    } else {
        DisplayInfoMessage(SYS_UI_MSG_INSTALL_COMPLETE);
    }
}

static void RunValidatedFMCBInstall(int port, int slot, int standard_ps2_type)
{
    unsigned int flags;
    int result;
    const char *warning;

    if (HasOldMultiInstall(port, slot)) {
        DisplayErrorMessage(SYS_UI_MSG_HAS_MULTI_INST);
        return;
    }

    warning = standard_ps2_type
                  ? "This card passed the filesystem and real MagicGate KELF-binding tests. Install FMCB using the validated path?"
                  : "The normal installer does not report this as a standard PS2 memory-card type, but filesystem R/W and real MagicGate KELF binding both passed.\n\nForce FMCB installation?\n\nOnly the card-type heuristic will be bypassed. Any write, space, filesystem or MagicGate error will still abort installation.";

    if (ShowMessageBox(SYS_UI_LBL_CANCEL, SYS_UI_LBL_INSTALL, -1, -1, warning, SYS_UI_LBL_WARNING) != 2)
        return;

    flags = 0;
    if (GetPs2Type() == PS2_SYSTEM_TYPE_PS2) {
        result = ShowMessageBox(SYS_UI_LBL_INST_TYPE_NORMAL,
                                SYS_UI_LBL_INST_TYPE_CRS_MDL,
                                SYS_UI_LBL_INST_TYPE_CRS_REG,
                                -1,
                                GetUIString(SYS_UI_MSG_INST_PROMPT_INST_TYPE),
                                SYS_UI_LBL_CONFIRM);
        if (result == 0)
            return;

        switch (result) {
            case 2:
                flags |= INSTALL_MODE_FLAG_CROSS_MODEL;
                break;
            case 3:
                flags |= INSTALL_MODE_FLAG_CROSS_REG;
                break;
        }
    }

    if (HasOldFMCBConfigFile(port, slot)) {
        result = DisplayPromptMessage(SYS_UI_MSG_CNF_FOUND, SYS_UI_LBL_YES, SYS_UI_LBL_NO);
        if (result == 1)
            flags |= INSTALL_MODE_FLAG_SKIP_CNF;
        else if (result == 0)
            return;
    }

    SetMagicGateValidatedOverride(port, slot, 1);
    result = PerformInstallation(port, slot, flags);
    SetMagicGateValidatedOverride(port, slot, 0);

    DisplayValidatedInstallResult(result);
}

static void RunMagicGateInspectorFlow(void)
{
    MGInspectorResult report;
    char report_text[2048];
    int selection;
    int result;

    selection = ShowMessageBox(SYS_UI_LBL_SLOT1,
                               SYS_UI_LBL_SLOT2,
                               -1,
                               -1,
                               "Select the memory-card slot to inspect.\n\nThe test writes and verifies one temporary 4 KiB file, deletes it, then binds FMCB.XLF in RAM using the installer's real MagicGate path.",
                               SYS_UI_LBL_MG_INSPECTOR);
    if (selection <= 0)
        return;

    memset(&report, 0, sizeof(report));
    result = MGInspectorRun(selection - 1, 0, &report);
    MGInspectorFormatReport(&report, report_text, sizeof(report_text));

    if (result == 0 && report.full_pass) {
        selection = ShowMessageBox(SYS_UI_LBL_OK,
                                   SYS_UI_LBL_INSTALL,
                                   -1,
                                   -1,
                                   report_text,
                                   SYS_UI_LBL_MG_INSPECTOR);
        if (selection == 2)
            RunValidatedFMCBInstall(report.port, report.slot, report.standard_ps2_type);
    } else {
        ShowMessageBox(SYS_UI_LBL_OK, -1, -1, -1, report_text, SYS_UI_LBL_MG_INSPECTOR);
    }
}

void MainMenu(void)
{
    int result;'''

    event_handler_old = '''        switch (event) {
            case EVENT_MULTI_INSTALL:'''
    event_handler_new = '''        switch (event) {
            case EVENT_MG_INSPECT:
                RunMagicGateInspectorFlow();
                break;
            case EVENT_MULTI_INSTALL:'''

    menu_edits = [
        ("include inspector", menu_include_old, menu_include_new),
        ("add menu id", menu_id_old, menu_id_new),
        ("add MC menu button", mc_menu_old, mc_menu_new),
        ("add description", callback_old, callback_new),
        ("map inspector event", event_map_old, event_map_new),
        ("add inspector and forced-install flow", helper_anchor, helper_code),
        ("handle inspector event", event_handler_old, event_handler_new),
    ]

    try:
        patch_file(installer / "Makefile", makefile_edits, args.dry_run)
        patch_file(installer / "lang.h", lang_h_edits, args.dry_run)
        patch_file(installer / "lang.c", lang_c_edits, args.dry_run)
        patch_file(installer / "system.h", system_h_edits, args.dry_run)
        patch_file(installer / "system.c", system_c_edits, args.dry_run)
        patch_file(installer / "menu.c", menu_edits, args.dry_run)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if not args.dry_run:
        overlay = Path(__file__).resolve().parent / "overlay" / "installer"
        shutil.copy2(overlay / "mg_inspector.c", installer / "mg_inspector.c")
        shutil.copy2(overlay / "mg_inspector.h", installer / "mg_inspector.h")

    action = "validated" if args.dry_run else "applied"
    print(f"{PATCH_NAME}: {action} successfully")
    if not args.dry_run:
        print("Backups were written as *.mgci.bak next to modified files.")
        print("Build using the upstream FreeMcBoot-Installer build workflow / PS2SDK toolchain.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
