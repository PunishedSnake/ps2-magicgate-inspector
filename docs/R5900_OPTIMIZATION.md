# Drebin R5900 / EE optimization notes

This document records the CPU-side optimization contract for Drebin. The goal is not to keep the EE implementation in portable C at all costs. Native R5900 assembly, MMI, DMAC and scratchpad techniques are valid tools when they produce a measured improvement and preserve the card/USB safety model.

The project deliberately separates four different kinds of cost:

1. memory-card/SIO2 transfer time on the IOP;
2. EE <-> IOP SIF RPC and DMA overhead;
3. USBHDFSD/fileXio transaction overhead;
4. work actually executed by the EE core, such as copying, CRC32 and regenerated PCSX2 ECC.

Optimizing category 4 cannot compensate for hundreds of thousands of avoidable RPCs in category 2. Likewise, reducing RPC traffic does not make a bit-at-a-time CRC implementation free. Each layer is measured independently.

## Primary references

The preferred architectural references are the archived Sony manuals collected by `ninjadynamics/PS2Docs`:

- `EE_Overview_Manual.pdf`
- `EE_Users_Manual.pdf`
- `EE_Core_Users_Manual.pdf`
- `EE_Core_Instruction_Set_Manual.pdf`

Repository: <https://github.com/ninjadynamics/PS2Docs>

The R5900/MMI instruction-set manual is the source of truth before introducing an EE-specific instruction sequence.

For toolchain behaviour, use the matching GNU GCC/GAS MIPS documentation. In particular GCC documents `-mfix-r5900`, the workaround for the R5900 short-loop hardware erratum. A branch at the end of a loop of six instructions or fewer must not receive a useful preceding instruction in its delay slot under the affected pattern; GAS can force a `nop` there. The current native Drebin loop uses an explicit `nop` and `.set noreorder` rather than depending on scheduler behaviour.

GCC reference: <https://gcc.gnu.org/onlinedocs/gcc/MIPS-Options.html>

PS2SDK itself treats assembly as a normal EE build input. Its `Makefile.eeglobal` has a `%.S -> %.o` rule, while EE kernel code also uses explicit `.set noreorder`, cache instructions and hand-written assembly where appropriate.

## Current optimization tiers

### Tier 1: remove transactions before optimizing instructions

Raw full-card acquisition originally issued one legacy `mcReadPage()` RPC and one `mcSync()` per 512-byte page. A 64 MiB card therefore required 131072 page-level EE/IOP round trips.

Drebin's private raw MCSERV extension batches up to sixteen pages in one call:

```text
16 * 512 = 8192 bytes
```

The IOP still invokes MCMAN page access for each page, so SIO2/card semantics, ECC correction and logical-port state stay where they were. Only the SIF dispatch/DMA granularity changes. A 64 MiB sequential acquisition can therefore approach 8192 bulk RPCs instead of 131072 page RPCs. The legacy path remains the fallback.

This is intentionally the first optimization layer because an instruction-level optimization cannot recover time spent repeatedly crossing EE/IOP boundaries.

### Tier 2: native aligned R5900 stream copies

`src/r5900_memops.S` implements the first explicitly native EE fast path:

```text
MciR5900CopyQwords
```

It uses 128-bit `LQ` / `SQ` on buffers whose alignment and size satisfy the qword contract. `MciFastCopy()` in `src/r5900_memops.h` checks those conditions and falls back to ordinary `memcpy()` otherwise.

Current users are deliberately narrow:

- sequential image read-ahead cache -> caller buffer;
- image record -> sequential write-behind cache.

The primitive is not a global replacement for libc `memcpy`. Keeping it local lets real hardware measurements decide whether the native path earns a wider role.

The loop processes two qwords, 32 bytes, per iteration and leaves the branch delay slot as an explicit `nop`. This also avoids relying on scheduling behaviour around the R5900 short-loop erratum.

### Tier 3: table-driven CRC32 and PCSX2 ECC

The current image engine still performs CRC32 bit by bit and calculates each ECC column contribution by testing all eight bits of every source byte. These are genuine EE hot loops after SIF/USB transaction counts are reduced.

The planned first replacement is intentionally cache-conscious rather than enormous:

- CRC32: 256-entry `u32` table, 1024 bytes;
- ECC: 256-entry packed lookup containing the column contribution and byte parity, 512 bytes if stored as `u16`;
- remove the eight-iteration `ColumnMask()` loop;
- remove the runtime parity folding;
- make line-parity accumulation branchless or otherwise benchmark both forms.

A very large slicing table is not automatically better on this machine. The EE has a small data cache, so table size, sequential source access and cache pressure must be measured rather than inferred from desktop CRC implementations.

Before the production path changes, host-side equivalence tests must prove identical output against the existing bitwise algorithms across all byte values and representative/random 512-byte pages.

### Tier 4: MMI / scratchpad / overlapped pipeline

MMI is not forbidden. It is reserved for loops where its 128-bit operations match the actual data dependency graph and beat the simpler scalar/table implementation on hardware.

The EE scratchpad is also a candidate for the acquisition pipeline. An especially attractive geometry is two 8192-byte working buffers, matching the current sixteen-page raw MCSERV batch. A future double-buffered design could conceptually overlap:

```text
IOP/SIO2: acquire batch N+1
EE:       CRC/ECC/format batch N
USB:      publish batch N-1
```

That design is intentionally not enabled until the simpler bulk RPC is qualified on real hardware. It introduces additional DMA/cache/lifetime interactions, and USBHDFSD has already demonstrated that seemingly harmless concurrent file activity can corrupt a long-lived image stream.

## Things we do not optimize away

The following are correctness boundaries, not performance bugs:

- MCMAN owns logical 0/1 -> physical SIO2 2/3 selection for ordinary/raw page I/O.
- SECRMAN/CardAuth has its own explicit logical -> physical boundary.
- source-ECC warnings remain distinguishable from genuine transport failures.
- completed images remain subject to reopen/read-back verification.
- destructive restore/format operations keep their geometry and recovery gates.
- DREBIN.LOG mass writes remain isolated from long-lived image descriptors.

Do not shorten undocumented SIO2/card timing or remove MCMAN retry behaviour merely because a benchmark gets faster. Such changes require separate protocol-level evidence and real-card qualification.

## Benchmark discipline

Every optimization should answer three questions:

1. Which layer did it reduce: SIO2, SIF, USB, or EE compute?
2. What counter/timing proves the reduction?
3. Did output remain bit-identical and did the safety/verification path still pass?

Useful counters already include raw bulk RPC calls, cache hits, fallbacks, pages fetched, ECC warnings and accumulated RPC ticks. CPU-side math and copy paths should gain similarly scoped counters only when the instrumentation cost is small enough not to distort the hot path.

The intended progression is therefore:

```text
remove needless transactions
    -> exploit aligned native R5900 loads/stores
    -> remove scalar bit-at-a-time work
    -> evaluate MMI and scratchpad overlap
    -> keep only changes that win on real hardware
```
