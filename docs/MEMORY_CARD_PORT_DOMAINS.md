# PS2 memory-card port domains

This document is the source of truth for memory-card port numbering in Drebin.

The short version is deliberately blunt:

> `mc0 = 0` and `mc1 = 1` are logical libmc/MCMAN ports. `mc0 = 2` and `mc1 = 3` are physical SIO2 card channels. Add `2` only at a boundary that explicitly consumes a physical SIO2 channel. Never add `2` merely because an operation eventually reaches a memory card.

This distinction has already caused two independent real-hardware failures in this project, in opposite directions. Treat it as an architectural contract, not an implementation detail.

## The two numeric domains

| Meaning | mc0 | mc1 | Used by |
| --- | ---: | ---: | --- |
| Logical memory-card port | `0` | `1` | EE libmc, MCSERV requests, MCMAN device state, filesystem code, raw page API callers |
| Physical SIO2 memory-card channel | `2` | `3` | CardAuth / SECRMAN callbacks and the low-level SIO2 transfer layer |

The same physical slot therefore has two valid numbers depending on the layer currently being addressed.

## Rule 1: ordinary libmc and MCMAN state stay on 0/1

Drebin code that calls APIs such as:

```c
mcGetInfo(port, ...);
mcOpen(port, ...);
mcReadPage(port, ...);
mcWritePage(port, ...);
mcEraseBlock(port, ...);
```

must pass logical ports `0` or `1`.

This includes the temporary legacy MCSERV used by Card Tools. Its MCMAN device state is initialized under logical indices 0/1. That state contains more than card presence: page size, block size, flags and filesystem geometry are attached to the logical device entry.

The MCMAN SIO2 packet builder later chooses the physical card channel itself from the logical port. Conceptually:

```text
EE/libmc port 0 or 1
        |
        v
MCSERV/MCMAN device state 0 or 1
        |
        v
MCMAN SIO2 packet builder
        |
        +--> physical channel (port & 1) + 2
             mc0 -> 2
             mc1 -> 3
```

Do not pre-convert raw page operations to 2/3 before MCMAN sees them.

## Rule 2: SECRMAN/CardAuth requires 2/3 at its boundary

SECRMAN's card-authentication callbacks consume a physical SIO2 memory-card channel, not a libmc logical port.

Drebin therefore keeps the application and libsecr side on logical 0/1, then translates exactly once where the card-port-bearing SECR RPC crosses into CardAuth:

```text
Drebin/libsecr logical port
        mc0 = 0
        mc1 = 1
             |
             v
SECRSIF boundary
             |
             +--> 2 + logical port
                  mc0 = 2
                  mc1 = 3
             |
             v
SECRMAN / CardAuth
```

The affected SECRSIF requests are currently:

```text
DOWNLOAD_HEADER
GET_KBIT
GET_KC
```

The implementation lives in `src/magicgate_diag.c` and uses the shared definitions in `src/mc_port.h`.

The reference FreeMcBoot binding path also enters this security layer using `2 + port`. That is why seeing `+2` in FMCB security code is correct while seeing the same conversion in ordinary libmc/raw-card code is not.

## Why this rule exists: hardware failure #1, missing +2 in MagicGate

The first Drebin/Inspector MagicGate implementation forwarded logical 0/1 directly into SECRMAN CardAuth.

Known-good Sony cards repeatedly failed at the first real card-side Kbit command with the signature:

```text
command 0x50
stat6c  0001D100
id      FF
status  FF
```

The problem was not the cards. CardAuth was being directed at controller-channel numbering rather than memory-card-channel numbering.

After translating only the SECR card-port-bearing requests:

```text
mc0 logical 0 -> SIO2/CardAuth 2
mc1 logical 1 -> SIO2/CardAuth 3
```

known-good cards immediately completed Kbit/Kc. The negative-control non-MagicGate card continued to fail at the genuine first CardAuth command, proving that the port correction fixed routing rather than merely hiding an error.

## Why this rule exists: hardware failure #2, extra +2 in raw imaging

Legacy PS2SDK MCSERV historically remapped raw erase/read/write operations from 0/1 to 2/3 before calling MCMAN.

With the MCMAN generation used by Drebin this was wrong. `mcGetInfo()` had initialized the complete `mcman_devinfos[]` state under logical indices 0/1, while the extra MCSERV remap made raw page operations consult a different 2/3 device-state entry.

That could leave the raw path with card presence detected but incomplete geometry such as page-size/card-flags state. Earlier Drebin builds therefore failed raw imaging at or near the first page.

The Drebin raw MCSERV patch now deliberately preserves logical 0/1 for:

```text
McReadPage
McWritePage
McEraseBlock
```

and lets MCMAN perform the physical SIO2 2/3 selection when it actually builds the transfer.

See `tools/patch_raw_mcserv_ports.py`.

## Source-code contract

`src/mc_port.h` defines the vocabulary used by new code:

```c
MCI_MC_LOGICAL_0
MCI_MC_LOGICAL_1
MCI_SIO2_CARD_0
MCI_SIO2_CARD_1

MciMcLogicalPortIsValid(...)
MciSio2CardPortIsValid(...)
MciMcLogicalToSio2CardPort(...)
MciSecrBoundaryCardPort(...)
```

`MciMcLogicalToSio2CardPort()` is intentionally strict: only logical 0/1 are valid input.

`MciSecrBoundaryCardPort()` exists only for the SECR RPC wrapper. It translates logical 0/1 and tolerates already-physical 2/3 solely to avoid double-shifting an external/reference caller. New Drebin-owned callers must remain logical until the SECR boundary.

## Code-review checklist

Any change involving a variable named `port`, `card_port`, `mc_port`, `slot` or SIO2 routing should answer these questions explicitly:

1. What numeric domain is this value currently in?
2. Does the called API expect logical libmc/MCMAN numbering or a physical SIO2 channel?
3. Which exact layer owns the conversion?
4. Has a previous layer already converted it?
5. If `+2`, `| 2` or `(port & 1) + 2` appears, is this the one and only logical-to-physical boundary?

If those questions cannot be answered from the API implementation, inspect the implementation before changing the number. Do not infer the domain from the fact that the operation concerns a memory card.

## Practical do / do not

Correct:

```c
/* Filesystem / libmc / raw MCMAN path */
mcGetInfo(logical_mc_port, ...);
mcReadPage(logical_mc_port, ...);

/* SECR/CardAuth boundary */
param->port = MciSecrBoundaryCardPort(logical_mc_port);
```

Wrong:

```c
/* Wrong: MCMAN has not asked for a physical channel here. */
mcReadPage(logical_mc_port + 2, ...);
```

Also wrong:

```c
/* Wrong: SECRMAN consumes physical SIO2 numbering. */
param->port = logical_mc_port;
```

The important rule is not "always add 2" or "never add 2". The rule is:

> Convert exactly once, at the layer where a logical memory-card identity becomes a physical SIO2 channel.
