from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
h = (ROOT / "overlay/installer/mg_inspector.h").read_text()
i = (ROOT / "overlay/installer/mg_inspector.c").read_text()
t = (ROOT / "overlay/installer/mg_pcsx2_test.c").read_text()

assert '#define MG_TEST_HARDWARE_ONLY  2' in h
assert 'int emulator_qualification_pass;' in h
assert 'int full_pass;' in h
assert 'out->full_pass = 0;' in i
assert 'return out->emulator_qualification_pass ? 0 : -EINVAL;' in i
assert 'if (result.full_pass)\n        return 22;' in t

for needle in [
    'mcFormat(port, slot);',
    'host:mgci-result.json',
    'MG_PCSX2_UNFORMATTED_REJECT',
    'MG_PCSX2_FORMATTED',
    'MG_PCSX2_FRESH_FORMAT',
    'result.cleanup_test != MG_TEST_PASS',
]:
    assert needle in t, needle

assert 'VerifyTestFileAbsent' in i
assert 'Temp-file cleanup' in i
assert 'SecrDownloadFile(2 + port, slot, kelf_buffer)' in i
print('MGCI emulator/hardware safety contract: PASS')
