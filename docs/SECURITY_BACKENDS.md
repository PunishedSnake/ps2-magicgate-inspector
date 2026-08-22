# Security backend provenance

PS2 Memory Card Inspector 0.2.0 uses one production security backend: **PS2SDK 2.0 SECRMAN 1.4** with matching PS2SDK 2.0 SECRSIF and card-side modules.

The earlier FreeMcBoot-compatible SECRMAN 1.3 path was essential during investigation, but it has been retired from the release build after the modern PS2SDK stack reproduced the same positive and negative hardware results.

## Production backend

Upstream repository:

```text
https://github.com/ps2dev/ps2sdk
```

Pinned security-source commit:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

Release security/card components:

```text
PS2SDK 2.0 SECRMAN 1.4
PS2SDK 2.0 SECRSIF
PS2SDK 2.0 freesio2
PS2SDK 2.0 freepad
PS2SDK 2.0 mcman
PS2SDK 2.0 mcserv  # staged/embedded for session plumbing, intentionally not started
```

The normal application personality is separate and continues to use Sony ROM `XSIO2MAN/XPADMAN/XMCMAN/XMCSERV`.

## Hardware result

The final backend was tested against the same capability matrix used to validate the earlier compatibility path:

| Card | Result |
| --- | --- |
| Sony 8 MB #1 | `FUNCTIONAL` |
| Sony 8 MB #2 | `FUNCTIONAL` |
| Third-party 64 MB with functional MagicGate | `FUNCTIONAL` |
| Third-party 64 MB without functional MagicGate | `NOT SUPPORTED / NO CARD AUTH ACK` |

Normal ROM X stack restoration also remained functional after the probes.

This is sufficient to make the PS2SDK 2.0 backend the release default and remove the legacy 1.3 build path from normal source/build plumbing.

## Port mapping requirement

PS2SDK CardAuth treats its `port` argument as an SIO2 channel. Therefore libmc logical ports must not be passed through unchanged:

```text
logical mc0 = 0 -> SIO2 card channel 2
logical mc1 = 1 -> SIO2 card channel 3
```

Inspector performs this translation only on SECRSIF requests that contain a memory-card port (`DOWNLOAD_HEADER`, `GET_KBIT`, `GET_KC`). Ordinary libmc calls remain 0/1.

This mapping is not a legacy workaround; it is a requirement of the CardAuth interface and remains necessary with SECRMAN 1.4.

## Build-time instrumentation

Stock PS2SDK 2.0 routes GET_KBIT through a private `scePreEncryptKbit()` helper. To diagnose the actual failing stage without replaying authentication commands, CI applies:

```text
tools/patch_secrman14_diag.py
```

to the temporary pinned PS2SDK checkout.

The patch:

- expands the two Mechacon pre-encryption half-key calls inside `SecrDownloadGetKbit()`;
- captures the real CardAuth transfer state for commands 0x50/0x51/0x52/0x53;
- records command, callback result, `stat6c`, card ID/status and checksum state;
- serializes that state into the failed 16-byte Kbit reply only when GET_KBIT fails;
- removes the now-unused helper/prototype so PS2SDK's `-Werror` build remains clean;
- leaves the successful GET_KBIT path on the upstream authentication flow.

No authentication command is replayed after failure.

## Historical SECRMAN 1.3 baseline

During Briscoe development, a pinned FreeMcBoot Installer compatibility revision was used to reproduce and diagnose the Kbit failure:

```text
FreeMcBoot-Installer commit
ac53a47a5c6eae675cc2611c7bebe62f56c7845c
```

That baseline established several important facts:

1. both Sony cards reached the same failing CardAuth path;
2. Mechacon pre-encryption completed (`pre=1/1`);
3. the first real CardAuth command failed with `stat6c=0001D100 id=FF st=FF`;
4. comparing the result with the reference FreeMcBoot call convention exposed the missing `2 + port` mapping;
5. correcting the mapping produced full PASS on known-good cards and a clean negative control on the non-MagicGate card.

After PS2SDK 2.0 SECRMAN 1.4 reproduced those same hardware outcomes, retaining a second production backend no longer provided enough value to justify its code and licensing complexity.

The 1.3 investigation remains documented in `CHANGELOG.md` and `docs/MAGICGATE.md` and is preserved in repository history.

## Licensing

PS2SDK is distributed under the **Academic Free License 2.0 (AFL-2.0)**. The release includes the upstream license text at:

```text
licenses/PS2SDK-AFL-2.0.txt
```

The exact upstream source location, pinned commit and modification method are also recorded in `THIRD_PARTY_NOTICES.md` and in the CI-generated `SOURCE_PROVENANCE.txt` file.
