#ifndef MCI_SETTINGS_H
#define MCI_SETTINGS_H

#include "video_mode.h"

typedef enum MciFsTestProfile {
    MCI_FS_TEST_QUICK = 0,
    MCI_FS_TEST_EXTENDED,
    MCI_FS_TEST_THOROUGH,
    MCI_FS_TEST_PROFILE_COUNT
} MciFsTestProfile;

typedef struct MciSettings {
    MciVideoMode video_mode;
    MciFsTestProfile fs_profile;
    int preserve_existing_cnfs;
    int verify_every_install_file;
} MciSettings;

void MciSettingsDefaults(MciSettings *settings);
const char *MciFsTestProfileName(MciFsTestProfile profile);
unsigned int MciFsTestProfileBytes(MciFsTestProfile profile);

#endif /* MCI_SETTINGS_H */
