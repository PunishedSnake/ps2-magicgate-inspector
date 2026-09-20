/* SPDX-License-Identifier: MIT */
/*
 * FMCB compatibility policy distilled from current FMCB/MechaPwn source plus
 * real-hardware qualifications documented in docs/FMCB_COMPATIBILITY_CORPUS.md.
 *
 * Keep this file intentionally boring. It maps observed capabilities to policy;
 * it must not grow model-number folklore. Unknown cases stay conservative and
 * are resolved by preflight probes wherever the hardware can answer directly.
 */

#include <string.h>

#include "fmcb_compat.h"

void FmcbCompatibilityEvaluate(const MciConsoleProfile *console,
                               FmcbCompatibilityPolicy *policy)
{
    if (policy == NULL)
        return;

    memset(policy, 0, sizeof(*policy));
    policy->profile_kind = FMCB_COMPAT_PROFILE_AMBIGUOUS;
    policy->native_boot = FMCB_NATIVE_BOOT_UNKNOWN;
    policy->include_cex_only_payloads = 1;
    policy->require_distinct_kelf_preflight = 1;

    if (console == NULL)
        return;

    policy->region_transition_unsafe = console->region_mismatch;
    policy->early_japan_update_paths =
        console->mg_folder_region == 'I' && console->rom_version <= 0x0120u;
    policy->rare_rom_warning =
        console->rom_version == 0x0180u || console->rom_version == 0x0210u;

    if (console->is_psx) {
        policy->profile_kind = FMCB_COMPAT_PROFILE_PSX_DESR;
        policy->native_boot = FMCB_NATIVE_BOOT_NOT_APPLICABLE;
        policy->psx_twin_sign_required = 1;
        return;
    }

    /* Current/reference FMCB rejects native system-update installation on ROM
     * 2.30+ because OSDSYS no longer autoboots the memory-card update path.
     * We record this as boot capability, not as a blanket ban on preparing a
     * card for another console or another bootstrap. */
    policy->native_boot =
        console->rom_version >= 0x0230u
            ? FMCB_NATIVE_BOOT_ROM230_BLOCKED
            : FMCB_NATIVE_BOOT_SUPPORTED;

    if (console->region_mismatch) {
        policy->profile_kind = FMCB_COMPAT_PROFILE_AMBIGUOUS;
        return;
    }

    if (console->rom_is_dex) {
        policy->profile_kind = FMCB_COMPAT_PROFILE_REAL_DEX;
        policy->include_cex_only_payloads = 0;
        return;
    }

    if (console->mechapwn_signature) {
        if (console->mechapwn_dex_mode) {
            policy->profile_kind = FMCB_COMPAT_PROFILE_MECHAPWN_DEX;
            policy->include_cex_only_payloads = 0;
        } else {
            policy->profile_kind = FMCB_COMPAT_PROFILE_MECHAPWN_CEX;
        }
        return;
    }

    if (console->mecha_dex_like) {
        /* Do not guess that every odd/DEX-like MechaCon state has MechaPwn's
         * exact KELF behavior. Retain CEX-only payloads and let the distinct
         * source bind-preflight prove or reject the plan before card writes. */
        policy->profile_kind = FMCB_COMPAT_PROFILE_UNQUALIFIED_DEX_LIKE;
        return;
    }

    policy->profile_kind = FMCB_COMPAT_PROFILE_RETAIL_CEX;
}

const char *FmcbCompatibilityProfileText(FmcbCompatProfileKind kind)
{
    switch (kind) {
        case FMCB_COMPAT_PROFILE_RETAIL_CEX:
            return "RETAIL CEX";
        case FMCB_COMPAT_PROFILE_REAL_DEX:
            return "REAL DEX";
        case FMCB_COMPAT_PROFILE_MECHAPWN_DEX:
            return "MECHAPWN DEX";
        case FMCB_COMPAT_PROFILE_MECHAPWN_CEX:
            return "MECHAPWN CEX";
        case FMCB_COMPAT_PROFILE_UNQUALIFIED_DEX_LIKE:
            return "UNQUALIFIED DEX-LIKE";
        case FMCB_COMPAT_PROFILE_PSX_DESR:
            return "PSX/DESR";
        case FMCB_COMPAT_PROFILE_AMBIGUOUS:
        default:
            return "AMBIGUOUS";
    }
}

const char *FmcbNativeBootStateText(FmcbNativeBootState state)
{
    switch (state) {
        case FMCB_NATIVE_BOOT_SUPPORTED:
            return "NATIVE SYSTEM-UPDATE BOOT";
        case FMCB_NATIVE_BOOT_ROM230_BLOCKED:
            return "ROM 2.30+ NATIVE FMCB BOOT BLOCKED";
        case FMCB_NATIVE_BOOT_NOT_APPLICABLE:
            return "NOT APPLICABLE";
        case FMCB_NATIVE_BOOT_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}
