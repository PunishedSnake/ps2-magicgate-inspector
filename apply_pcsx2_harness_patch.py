#!/usr/bin/env python3
from __future__ import annotations
import argparse, shutil, sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {n}")
    return text.replace(old, new, 1)


def main() -> int:
    ap = argparse.ArgumentParser(description='Add non-interactive PCSX2 qualification mode to an already MGCI-patched FMCB installer tree.')
    ap.add_argument('repo', type=Path)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    installer = a.repo.resolve()/'installer'
    makefile = installer/'Makefile'
    main_c = installer/'main.c'
    if not makefile.exists() or not main_c.exists():
        print('error: installer/Makefile or installer/main.c missing', file=sys.stderr); return 2

    mf = makefile.read_text()
    mf = replace_once(
        mf,
        'EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system.o mg_inspector.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o',
        'EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system.o mg_inspector.o mg_pcsx2_test.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o',
        'Makefile MGCI object list')

    src = main_c.read_text()
    src = replace_once(src, '#include "menu.h"\n', '#include "menu.h"\n#include "mg_pcsx2_test.h"\n', 'main include')
    src = replace_once(
        src,
        '    int SystemType, InitSemaID, BootDevice, result;\n    unsigned int FrameNum;',
        '    int SystemType, InitSemaID, BootDevice, result;\n    int MGPCSX2TestMode = MGPCSX2TestRequested(argc, argv);\n    unsigned int FrameNum;',
        'test mode declaration')
    src = replace_once(
        src,
        '    if ((BootDevice = GetBootDeviceID()) == BOOT_DEVICE_UNKNOWN)\n',
        '    if (!MGPCSX2TestMode && (BootDevice = GetBootDeviceID()) == BOOT_DEVICE_UNKNOWN)\n',
        'skip USB gate in test mode')
    src = replace_once(
        src,
        '    InitSemaID = IopInitStart(IOP_MOD_SET_MAIN);\n',
        '    InitSemaID = IopInitStart(MGPCSX2TestMode ? IOP_LIBSECR_IMG : IOP_MOD_SET_MAIN);\n\n'
        '    if (MGPCSX2TestMode) {\n'
        '        while (PollSema(InitSemaID) != InitSemaID)\n'
        '            DelayThread(1000);\n'
        '        DeleteSema(InitSemaID);\n'
        '        result = MGPCSX2Run(argc, argv);\n'
        '        IopDeinit();\n'
        '        return result;\n'
        '    }\n',
        'noninteractive PCSX2 test path')

    if not a.dry_run:
        makefile.write_text(mf)
        main_c.write_text(src)
        overlay = Path(__file__).resolve().parent/'overlay'/'installer'
        shutil.copy2(overlay/'mg_pcsx2_test.c', installer/'mg_pcsx2_test.c')
        shutil.copy2(overlay/'mg_pcsx2_test.h', installer/'mg_pcsx2_test.h')
    print('PCSX2 MGCI harness patch: validated' if a.dry_run else 'PCSX2 MGCI harness patch: applied')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
