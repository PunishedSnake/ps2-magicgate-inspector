#ifndef MCI_SETTINGS_H
#define MCI_SETTINGS_H

#include "video_mode.h"

typedef enum MciFsTestProfile {
    MCI_FS_TEST_QUICK = 0,
    MCI_FS_TEST_EXTENDED,
    MCI_FS_TEST_THOROUGH,
    MCI_FS_TEST_PROFILE_COUNT
} MciFsTestProfile;

typedef enum MciInstallVerifyMode {
    MCI_INSTALL_VERIFY_ENFORCED = 0,
    MCI_INSTALL_VERIFY_REQUIRED,
    MCI_INSTALL_VERIFY_DISABLED,
    MCI_INSTALL_VERIFY_MODE_COUNT
} MciInstallVerifyMode;

typedef struct MciSettings {
    MciVideoMode video_mode;
    MciFsTestProfile fs_profile;
    int preserve_existing_cnfs;
    MciInstallVerifyMode install_verify_mode;
} MciSettings;

void MciSettingsDefaults(MciSettings *settings);
const char *MciFsTestProfileName(MciFsTestProfile profile);
unsigned int MciFsTestProfileBytes(MciFsTestProfile profile);
const char *MciInstallVerifyModeName(MciInstallVerifyMode mode);

#define MCI_SETTINGS_CONFIG_VERSION 1
#define MCI_SETTINGS_CONFIG_PATH_MAX 128

/*
 * Text configuration persisted on USB as MCI/MCINSPECTOR.CFG.
 * Unknown keys are ignored for forward compatibility. Unsupported versions or
 * invalid values never partially replace the caller's current settings.
 */
int MciSettingsLoadFromMass(MciSettings *settings,
                            char *loaded_path, unsigned int loaded_path_size);
int MciSettingsSaveToMass(const MciSettings *settings,
                          char *saved_path, unsigned int saved_path_size);

#endif /* MCI_SETTINGS_H */
