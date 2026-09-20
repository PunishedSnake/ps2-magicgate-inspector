#ifndef MCI_FMCB_COMPAT_H
#define MCI_FMCB_COMPAT_H

#include "console_profile.h"

/*
 * Capability-driven FMCB compatibility policy.
 *
 * This layer deliberately does not key installation behavior from an SCPH
 * sticker/model number. ROMVER, PSXVER, read-only MechaCon state and a positive
 * MechaPwn fingerprint are the inputs that actually affect OSDSYS lookup and
 * KELF policy. Unknown modifications remain conservative and are qualified by
 * real KELF bind probes before the first card write.
 */
typedef enum FmcbCompatProfileKind {
    FMCB_COMPAT_PROFILE_RETAIL_CEX = 0,
    FMCB_COMPAT_PROFILE_REAL_DEX,
    FMCB_COMPAT_PROFILE_MECHAPWN_DEX,
    FMCB_COMPAT_PROFILE_MECHAPWN_CEX,
    FMCB_COMPAT_PROFILE_UNQUALIFIED_DEX_LIKE,
    FMCB_COMPAT_PROFILE_PSX_DESR,
    FMCB_COMPAT_PROFILE_AMBIGUOUS
} FmcbCompatProfileKind;

typedef enum FmcbNativeBootState {
    FMCB_NATIVE_BOOT_UNKNOWN = 0,
    FMCB_NATIVE_BOOT_SUPPORTED,
    FMCB_NATIVE_BOOT_ROM230_BLOCKED,
    FMCB_NATIVE_BOOT_NOT_APPLICABLE
} FmcbNativeBootState;

typedef struct FmcbCompatibilityPolicy {
    FmcbCompatProfileKind profile_kind;
    FmcbNativeBootState native_boot;

    /* CEX-only payloads currently means ENDVDPL. Reference FMCB omits this
     * class for DEX. MechaPwn DEX is promoted only with a positive fingerprint
     * plus the DEX-mode signal proven on real hardware. */
    int include_cex_only_payloads;

    /* Installer behavior/capability notes, not sticker-model guesses. */
    int require_distinct_kelf_preflight;
    int early_japan_update_paths;
    int rare_rom_warning;
    int region_transition_unsafe;
    int psx_twin_sign_required;
} FmcbCompatibilityPolicy;

void FmcbCompatibilityEvaluate(const MciConsoleProfile *console,
                               FmcbCompatibilityPolicy *policy);
const char *FmcbCompatibilityProfileText(FmcbCompatProfileKind kind);
const char *FmcbNativeBootStateText(FmcbNativeBootState state);

#endif /* MCI_FMCB_COMPAT_H */
