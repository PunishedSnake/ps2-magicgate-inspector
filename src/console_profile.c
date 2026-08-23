/* SPDX-License-Identifier: MIT */
/*
 * Read-only console/region profile for FMCB planning.
 *
 * FMCB's system-update location is selected by the active ROMVER region, but
 * Dragon/Deckard consoles can have their effective regional policy changed by
 * tools such as MechaPwn. A ROMVER-only installer therefore cannot tell a
 * stock retail console from a DEX-like or region-modified runtime.
 *
 * This module deliberately does not contain MechaPwn write code and does not
 * alter NVRAM. It uses only public PS2SDK CDVD RPCs:
 *   - SCMD 0x03/00 for the MechaCon version;
 *   - SCMD 0x36 for a read-only regional data block where supported;
 *   - SCMD 0x0A to read five NVM words used as a MechaPwn fingerprint.
 *
 * The low bit of the Dragon MechaCon minor revision is kept as a DEX-like
 * capability signal. In addition, MechaPwn's region patch leaves the literal
 * key seed "MechaPwn\0 EC" in NVM words 227..231. Matching both signals lets
 * the planner distinguish a known MechaPwn DEX-like setup from real DEX or an
 * unrelated DEX-like configuration without guessing from a console sticker.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libcdvd.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <stdlib.h>
#include <string.h>

#include "console_profile.h"

/* MechaPwn writes these bytes through little-endian u16 loads before its NVM
 * routine serializes each word high-byte first. ReadNVM therefore returns the
 * following numeric words. This is a fingerprint only; no ciphertext or write
 * path is reproduced here. */
static const u16 MechaPwnKeySeed[MCI_MECHAPWN_KEYSEED_WORDS] = {
    0x654Du, 0x6863u, 0x5061u, 0x6E77u, 0xEC00u
};

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

static int ReadNvmWord(u16 offset, u16 *value)
{
    u8 input[2];
    u8 output[16];

    if (value == NULL)
        return -1;
    input[0] = (u8)(offset >> 8);
    input[1] = (u8)offset;
    memset(output, 0, sizeof(output));
    if (!sceCdApplySCmd(0x0A, input, sizeof(input), output))
        return -1;
    if (output[0] != 0)
        return -2;
    *value = (u16)(((u16)output[1] << 8) | output[2]);
    return 0;
}

static int ProbeMechaPwnSeed(MciConsoleProfile *profile)
{
    unsigned int i;
    int matched = 1;

    profile->mechapwn_signature = 0;
    memset(profile->nvm_keyseed, 0, sizeof(profile->nvm_keyseed));
    for (i = 0; i < MCI_MECHAPWN_KEYSEED_WORDS; i++) {
        int rc = ReadNvmWord((u16)(227u + i), &profile->nvm_keyseed[i]);
        if (rc < 0)
            return -30 - (int)i;
        if (profile->nvm_keyseed[i] != MechaPwnKeySeed[i])
            matched = 0;
    }
    profile->mechapwn_signature = matched;
    return 0;
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
     * MechaCon minor revision (for example 5.8/5.9). This remains a capability
     * signal, not a product-name detector. */
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

            /* Keep these bytes as diagnostics only. A stock North-American
             * retail parameter set can also contain 'A' in both positions, so
             * A/A by itself is NOT evidence of MechaPwn, DEX or region unlock. */
            profile->region_params_romver = (char)profile->region_params[0];
            profile->region_params_ps1 = (char)profile->region_params[5];
            profile->region_params_all_region =
                profile->region_params_romver == 'A' &&
                profile->region_params_ps1 == 'A';
        } else {
            profile->region_params_rc = -21;
        }
    }

    profile->nvm_keyseed_probe_rc = -40;
    if (profile->is_dragon)
        profile->nvm_keyseed_probe_rc = ProbeMechaPwnSeed(profile);

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
    profile->nvm_keyseed_probe_rc = -99;
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
    profile->region_mismatch = 0;

    if (profile->rom_is_dex) {
        profile->unlock_kind = MCI_REGION_UNLOCK_REAL_DEX;
        profile->region_unlocked = 1;
    } else if (profile->mechapwn_signature) {
        profile->unlock_kind = MCI_REGION_UNLOCK_MECHAPWN;
        profile->mechapwn_dex_mode = profile->mecha_dex_like;
        profile->mechapwn_cex_region_override = !profile->mecha_dex_like;

        if (profile->mecha_dex_like)
            profile->region_unlocked = 1;

        if (profile->is_deckard) {
            if (profile->mecha_dex_like) {
                /* MechaPwn's Deckard DEX policy uses the A system-update
                 * region. If the patch is visible but ROMVER is still another
                 * region, treat it as a pre-reboot/transitional state rather
                 * than writing to a folder that may stop being active. */
                if (profile->romver_region != 'A') {
                    profile->region_mismatch = 1;
                    profile->unlock_kind = MCI_REGION_UNLOCK_AMBIGUOUS;
                    profile->region_unlocked = 0;
                }
            } else {
                /* Deckard CEX can move the system-update region when the user
                 * changes MechaPwn region. A durable installer must cover all
                 * regions instead of installing only today's ROMVER folder. */
                profile->cross_region_required = 1;
            }
        }
    } else if (profile->mecha_probe_rc == 0 && profile->mecha_dex_like) {
        profile->unlock_kind = MCI_REGION_UNLOCK_DEX_LIKE;
        profile->region_unlocked = 1;
    }

    profile->compact_region_safe = profile->region_unlocked &&
                                   !profile->cross_region_required &&
                                   !profile->region_mismatch &&
                                   profile->mg_folder_region != '?';

    return profile->mg_folder_region == '?' ? -3 : 0;
}

const char *MciRegionUnlockKindText(MciRegionUnlockKind kind)
{
    switch (kind) {
        case MCI_REGION_UNLOCK_REAL_DEX: return "REAL DEX";
        case MCI_REGION_UNLOCK_MECHAPWN: return "MECHAPWN REGION PATCH";
        case MCI_REGION_UNLOCK_DEX_LIKE: return "DEX-LIKE / REGION UNLOCKED";
        case MCI_REGION_UNLOCK_REGION_PARAMS: return "REGION PARAMETER PROFILE";
        case MCI_REGION_UNLOCK_AMBIGUOUS: return "AMBIGUOUS REGION STATE";
        default: return "RETAIL / REGION-LOCKED";
    }
}

const char *MciConsoleRegionPolicyText(const MciConsoleProfile *profile)
{
    if (profile == NULL)
        return "UNKNOWN";
    if (profile->region_mismatch)
        return "MECHAPWN DEX TRANSITION / REBOOT REQUIRED";
    if (profile->rom_is_dex)
        return "ACTIVE ROMVER + REAL DEX";
    if (profile->mechapwn_signature) {
        if (profile->is_deckard && profile->mechapwn_dex_mode)
            return "MECHAPWN DECKARD DEX / A REGION";
        if (profile->is_deckard && profile->cross_region_required)
            return "MECHAPWN DECKARD CEX / CROSS-REGION";
        if (profile->mechapwn_dex_mode)
            return "MECHAPWN DEX-LIKE / ACTIVE ROMVER";
        return "MECHAPWN CEX REGION OVERRIDE";
    }
    if (profile->mecha_dex_like)
        return "ACTIVE ROMVER + DEX-LIKE MECHACON";
    if (profile->mecha_probe_rc < 0)
        return "ACTIVE ROMVER; MECHACON UNAVAILABLE";
    return "ACTIVE ROMVER + RETAIL MECHACON";
}
