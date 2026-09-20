#ifndef MCI_MC_PORT_H
#define MCI_MC_PORT_H

/*
 * Memory-card port domains used by Drebin.
 *
 * There are two different numeric domains that both describe the same physical
 * card slot:
 *
 *   logical/libmc domain     mc0 = 0, mc1 = 1
 *   physical SIO2 domain     mc0 = 2, mc1 = 3
 *
 * IMPORTANT: do not add 2 merely because an operation eventually reaches the
 * memory-card SIO2 channel. The conversion belongs only at an API boundary
 * that explicitly consumes a physical SIO2 channel number.
 *
 * Ordinary libmc, MCSERV and MCMAN device-state calls stay in the logical 0/1
 * domain. MCMAN's SIO2 packet builder performs the physical 2/3 selection when
 * it actually constructs the transfer.
 *
 * SECRMAN/CardAuth is different: its card-port-bearing RPCs consume the physical
 * SIO2 channel directly, so that boundary must translate 0/1 -> 2/3 exactly
 * once. See docs/MEMORY_CARD_PORT_DOMAINS.md.
 */

typedef enum MciMcLogicalPort {
    MCI_MC_LOGICAL_0 = 0,
    MCI_MC_LOGICAL_1 = 1
} MciMcLogicalPort;

typedef enum MciSio2CardPort {
    MCI_SIO2_CARD_0 = 2,
    MCI_SIO2_CARD_1 = 3
} MciSio2CardPort;

static inline int MciMcLogicalPortIsValid(int port)
{
    return port == MCI_MC_LOGICAL_0 || port == MCI_MC_LOGICAL_1;
}

static inline int MciSio2CardPortIsValid(int port)
{
    return port == MCI_SIO2_CARD_0 || port == MCI_SIO2_CARD_1;
}

/* Strict logical -> physical conversion. Invalid/non-logical input is rejected. */
static inline int MciMcLogicalToSio2CardPort(int logical_port)
{
    if (!MciMcLogicalPortIsValid(logical_port))
        return -1;
    return logical_port + 2;
}

/*
 * SECR RPC compatibility helper.
 *
 * Drebin-owned callers are expected to pass logical 0/1 into libsecr/SECRSIF.
 * The wrapper translates those values here. Already-physical 2/3 is tolerated
 * only so an external/reference caller is not double-shifted by the wrapper.
 * New Drebin code must not use 2/3 before the SECR/CardAuth boundary.
 */
static inline int MciSecrBoundaryCardPort(int caller_port)
{
    int physical_port = MciMcLogicalToSio2CardPort(caller_port);

    if (physical_port >= 0)
        return physical_port;
    if (MciSio2CardPortIsValid(caller_port))
        return caller_port;
    return caller_port;
}

#endif /* MCI_MC_PORT_H */
