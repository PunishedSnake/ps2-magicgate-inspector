#ifndef MCI_CONSOLE_PROFILE_H
#define MCI_CONSOLE_PROFILE_H

#include <tamtypes.h>

#define MCI_ROMVER_TEXT_MAX 17
#define MCI_MECHA_REGION_PARAMS_SIZE 13

typedef enum MciRegionUnlockKind {
    MCI_REGION_UNLOCK_NONE = 0,
    MCI_REGION_UNLOCK_REAL_DEX,
    MCI_REGION_UNLOCK_DEX_LIKE,
    MCI_REGION_UNLOCK_REGION_PARAMS,
    MCI_REGION_UNLOCK_AMBIGUOUS
} MciRegionUnlockKind;

typedef struct MciConsoleProfile {
    char romver[MCI_ROMVER_TEXT_MAX];
    unsigned int rom_version;
    char romver_region;
    char mg_folder_region;
    int rom_is_dex;
    int is_psx;

    int mecha_probe_rc;
    u8 mecha_version[4];
    unsigned int mecha_major;
    unsigned int mecha_minor_raw;
    unsigned int mecha_minor;
    unsigned int mecha_region_selector;
    int is_dragon;
    int is_slim;
    int is_deckard;
    int mecha_dex_like;

    int region_params_rc;
    u8 region_params[MCI_MECHA_REGION_PARAMS_SIZE];
    char region_params_romver;
    char region_params_ps1;
    int region_params_all_region;
    int region_mismatch;

    MciRegionUnlockKind unlock_kind;
    int region_unlocked;
    int compact_region_safe;
} MciConsoleProfile;

/* Read-only probe. ROMVER/PSXVER come from rom0: while MechaCon state is read
 * with public PS2SDK CDVD RPCs. No NVRAM/config write command is issued. */
int MciConsoleProfileProbe(MciConsoleProfile *profile);

char MciMgFolderFromRomRegion(char region);
const char *MciRegionUnlockKindText(MciRegionUnlockKind kind);
const char *MciConsoleRegionPolicyText(const MciConsoleProfile *profile);

#endif /* MCI_CONSOLE_PROFILE_H */
