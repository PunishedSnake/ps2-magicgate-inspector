#!/usr/bin/env python3
"""Source invariants for persistent MC Inspector settings."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

settings_c = (ROOT / "src/settings.c").read_text(encoding="utf-8")
settings_h = (ROOT / "src/settings.h").read_text(encoding="utf-8")
app_c = (ROOT / "src/app_main.c").read_text(encoding="utf-8")
app_v2 = (ROOT / "src/app_main_v2.c").read_text(encoding="utf-8")
gui = (ROOT / "src/gui_core.inc").read_text(encoding="utf-8")
doc = (ROOT / "docs/SETTINGS_CONFIG.md").read_text(encoding="utf-8")

assert "#define MCI_SETTINGS_CONFIG_VERSION 1" in settings_h
assert "MciSettingsLoadFromMass" in settings_h
assert "MciSettingsSaveToMass" in settings_h

for key in (
    "version=",
    "video_mode=",
    "fs_profile=",
    "preserve_existing_cnfs=",
    "install_verify_mode=",
):
    assert key in settings_c, f"missing config key {key}"

for value in ("native", "480p", "576p", "720p", "1080i"):
    assert value in settings_c or value in (ROOT / "src/video_mode.c").read_text(encoding="utf-8")

assert "MCINSPECTOR.CFG.tmp" not in settings_c  # path is composed as <final>.tmp
assert 'snprintf(temp, sizeof(temp), "%s.tmp", path)' in settings_c
assert "ReadConfigFile(temp, verify" in settings_c
assert "memcmp(buffer, verify" in settings_c
assert "fileXioRename(temp, path)" in settings_c
assert "fileXioSetBlockMode(FXIO_WAIT)" in settings_c
assert "MciSettingsDefaults" in settings_c

for app in (app_c, app_v2):
    assert "LoadSavedSettingsAfterMass(&last_video_rc)" in app
    assert "MciSettingsSaveToMass" in app_c  # helper is included by v2
    assert "PAD_SQUARE" in app and "MCI_GUI_SETTINGS" in app
    assert "SETTINGS SAVED" in app

assert "SQUARE Save CFG" in gui
assert "version=1" in doc
assert "video_mode=native" in doc
assert "install_verify_mode=enforced" in doc

print("Settings config invariants: PASS")
