#include "settings.h"

void MciSettingsDefaults(MciSettings *settings)
{
    if (settings == 0)
        return;
    settings->video_mode = MCI_VIDEO_NATIVE;
    settings->fs_profile = MCI_FS_TEST_QUICK;
    settings->preserve_existing_cnfs = 1;
    settings->install_verify_mode = MCI_INSTALL_VERIFY_ENFORCED;
}

const char *MciFsTestProfileName(MciFsTestProfile profile)
{
    switch (profile) {
        case MCI_FS_TEST_EXTENDED: return "Extended (64 KiB)";
        case MCI_FS_TEST_THOROUGH: return "Thorough (256 KiB)";
        case MCI_FS_TEST_QUICK:
        default: return "Quick (4 KiB)";
    }
}

unsigned int MciFsTestProfileBytes(MciFsTestProfile profile)
{
    switch (profile) {
        case MCI_FS_TEST_EXTENDED: return 64u * 1024u;
        case MCI_FS_TEST_THOROUGH: return 256u * 1024u;
        case MCI_FS_TEST_QUICK:
        default: return 4u * 1024u;
    }
}

const char *MciInstallVerifyModeName(MciInstallVerifyMode mode)
{
    switch (mode) {
        case MCI_INSTALL_VERIFY_REQUIRED: return "REQUIRED ONLY";
        case MCI_INSTALL_VERIFY_DISABLED: return "DISABLED";
        case MCI_INSTALL_VERIFY_ENFORCED:
        default: return "ENFORCED";
    }
}
