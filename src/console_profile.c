/* SPDX-License-Identifier: MIT */
/*
 * Read-only console/region profile for FMCB planning.
 *
 * FMCB's system-update location is selected by the active ROMVER region, but
 * Dragon/Deckard consoles can have their effective regional policy changed by
 * tools such as MechaPwn. A ROMVER-only installer therefore cannot tell a
 * stock retail console from a DEX-like/all-region runtime.
 *
 * This module deliberately does not contain MechaPwn code and does not touch
 * NVRAM. It uses only public PS2SDK CDVD RPCs:
 *   - SCMD 0x03/00 for the MechaCon version;
 *   - SCMD 0x36 for the read-only region-parameter block where supported.
 *
 * The low bit of the Dragon MechaCon minor revision is treated as a DEX-like
 * indicator. Existing PS2 diagnostics canonicalize e.g. 5.9 to 5.8 for this
 * reason; combined with ROMVER and, on slim units, region parameters it gives
 * us a conservative signal rather than blindly assuming the sticker region.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libcdvd.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <stdlib.h>
#include <string.h>

#include "console_profile.h"

static int ReadRomText(const char *path, char *buffer, unsigned int size)
{
    int fd;
    int rc;

    if (buffer == NULL || size < 2u)
        return -1;
    memset(buffer, 0, size);
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    rc = fileXioRead(fd, buffer, (int)size - 1);
    fileXioClose(fd);
    if (rc < 0)
        return rc;
    if ((unsigned int)rc >= size)
        rc = (int)size - 1;
    buffer[rc] = '\0';
    return rc;
}

char MciMgFolderFromRomRegion(char region)
{
    switch (region) {
        case 'J': return 'I';
        case 'A':
        case 'H': return 'A';
        case 'E': return 'E';
        case 'C': return 'C';
        default: return '?';
    }
}

static int ProbeMecha(MciConsoleProfile *profile)
{
    u8 subcommand = 0;
    u8 output[16];
    int initialized;

    memset(output, 0, sizeof(output));
    initialized = sceCdInit(SCECdINoD);
    if (!initialized)
        return -10;

    if (!sceCdApplySCmd(0x03, &subcommand, 1, output)) {
        sceCdInit(SCECdEXIT);
        return -11;
    }
    if ((output[0] & 0x80u) != 0u) {
        sceCdInit(SCECdEXIT);
        return -12;
    }

    memcpy(profile->mecha_version, output, sizeof(profile->mecha_version));
    profile->mecha_version[0] &= 0x7Fu;
    profile->mecha_major = profile->mecha_version[1];
    profile->mecha_minor_raw = profile->mecha_version[2];
    profile->mecha_minor = profile->mecha_minor_raw & ~1u;
    profile->mecha_region_selector = profile->mecha_version[0] & 7u;
    profile->is_dragon = profile->mecha_major >= 5u;
    profile->is_slim = profile->mecha_major == 6u;
    profile->is_deckard = profile->is_slim && profile->mecha_minor >= 6u;

    /* Dragon DEX/QA configurations expose the odd partner of the retail
     * MechaCon minor revision (for example 5.8/5.9). We do not call this
     * "MechaPwn detected": real DEX hardware and other patches can present the
     * same effective state. For an installer the capability is what matters. */
    profile->mecha_dex_like = profile->is_dragon &&
                              ((profile->mecha_minor_raw & 1u) != 0u);

    profile->region_params_rc = -20;
    if (profile->mecha_major >= 6u) {
        memset(output, 0, sizeof(output));
        if (sceCdApplySCmd(0x36, NULL, 0, output) &&
            (output[0] & 0x80u) == 0u) {
            memcpy(profile->region_params, output + 1,
                   MCI_MECHA_REGION_PARAMS_SIZE);
            profile->region_params_rc = 0;
            profile->region_params_romver = (char)profile->region_params[0];
            profile->region_params_ps1 = (char)profile->region_params[5];
            profile->region_params_all_region =
                profile->region_params_romver == 'A' &&
                profile->region_params_ps1 == 'A';
        } else {
            profile->region_params_rc = -21;
        }
    }

    sceCdInit(SCECdEXIT);
    return 0;
}

int MciConsoleProfileProbe(MciConsoleProfile *profile)
{
    char version_text[5];
    char psxver[8];
    int rc;

    if (profile == NULL)
        return -1;
    memset(profile, 0, sizeof(*profile));
    profile->romver_region = '?';
    profile->mg_folder_region = '?';
    profile->mecha_probe_rc = -99;
    profile->region_params_rc = -99;
    profile->region_params_romver = '?';
    profile->region_params_ps1 = '?';
    profile->unlock_kind = MCI_REGION_UNLOCK_NONE;

    rc = ReadRomText("rom0:ROMVER", profile->romver,
                     sizeof(profile->romver));
    if (rc < 6)
        return -2;

    memcpy(version_text, profile->romver, 4);
    version_text[4] = '\0';
    profile->rom_version = (unsigned int)strtoul(version_text, NULL, 16);
    profile->romver_region = profile->romver[4];
    profile->mg_folder_region = MciMgFolderFromRomRegion(profile->romver_region);
    profile->rom_is_dex = profile->romver[5] == 'D';
    profile->is_psx = ReadRomText("rom0:PSXVER", psxver, sizeof(psxver)) >= 0;

    profile->mecha_probe_rc = ProbeMecha(profile);
    if (profile->mecha_probe_rc == 0 && profile->region_params_rc == 0 &&
        profile->region_params_romver != '\0' &&
        profile->region_params_romver != profile->romver_region) {
        profile->region_mismatch = 1;
    }

    if (profile->rom_is_dex) {
        profile->unlock_kind = MCI_REGION_UNLOCK_REAL_DEX;
        profile->region_unlocked = 1;
    } else if (profile->mecha_probe_rc == 0 && profile->mecha_dex_like) {
        profile->unlock_kind = MCI_REGION_UNLOCK_DEX_LIKE;
        profile->region_unlocked = 1;
    } else if (profile->region_params_rc == 0 &&
               profile->region_params_all_region) {
        profile->unlock_kind = MCI_REGION_UNLOCK_REGION_PARAMS;
        profile->region_unlocked = 1;
    }

    if (profile->region_mismatch) {
        profile->unlock_kind = MCI_REGION_UNLOCK_AMBIGUOUS;
        profile->compact_region_safe = 0;
    } else {
        profile->compact_region_safe = profile->region_unlocked &&
                                       profile->mg_folder_region != '?';
    }

    return profile->mg_folder_region == '?' ? -3 : 0;
}

const char *MciRegionUnlockKindText(MciRegionUnlockKind kind)
{
    switch (kind) {
        case MCI_REGION_UNLOCK_REAL_DEX: return "REAL DEX";
        case MCI_REGION_UNLOCK_DEX_LIKE: return "DEX-LIKE / REGION UNLOCKED";
        case MCI_REGION_UNLOCK_REGION_PARAMS: return "ALL-REGION PARAMETERS";
        case MCI_REGION_UNLOCK_AMBIGUOUS: return "AMBIGUOUS REGION STATE";
        default: return "RETAIL / REGION-LOCKED";
    }
}

const char *MciConsoleRegionPolicyText(const MciConsoleProfile *profile)
{
    if (profile == NULL)
        return "UNKNOWN";
    if (profile->region_mismatch)
        return "ROMVER / MECHACON MISMATCH";
    if (profile->compact_region_safe)
        return "ACTIVE REGION + UNLOCKED KELF POLICY";
    if (profile->region_unlocked)
        return "UNLOCKED, CONSERVATIVE INSTALL";
    return "ACTIVE ROMVER REGION";
}
