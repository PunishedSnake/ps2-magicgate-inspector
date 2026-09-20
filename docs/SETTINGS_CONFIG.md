# MCINSPECTOR.CFG

PS2 Memory Card Inspector stores persistent user preferences in a small,
versioned text file on USB.

Default search/save locations, in order:

```text
mass:/MCI/MCINSPECTOR.CFG
mass0:/MCI/MCINSPECTOR.CFG
mass1:/MCI/MCINSPECTOR.CFG
```

If an existing config is found, Save writes back to the same device. Otherwise
the first writable mass root is used.

## Format

Current version:

```ini
# PS2 Memory Card Inspector settings
version=1
video_mode=native
fs_profile=quick
preserve_existing_cnfs=1
install_verify_mode=enforced
```

Supported values:

```text
video_mode=
  native
  480p
  576p
  720p
  1080i

fs_profile=
  quick
  extended
  thorough

preserve_existing_cnfs=
  0
  1

install_verify_mode=
  enforced
  required
  disabled
```

Unknown keys are ignored so future versions can add settings without breaking
older builds.

An unsupported config version or an invalid value causes the entire load to be
rejected. The caller's current/default settings are retained; the parser never
partially applies a malformed file.

## Runtime behavior

At boot the application initializes the GUI in Native mode first. After the
normal USB backend is ready, it searches for `MCINSPECTOR.CFG`, loads the
settings and then applies the saved video mode.

This ordering is deliberate: a missing/corrupt config or a video mode that fails
to apply cannot prevent the recovery-safe Native UI from appearing.

The Settings page remains live/in-memory until the user presses:

```text
SQUARE  Save CFG
```

Saving uses:

1. synchronous `FXIO_WAIT`;
2. a temporary `MCINSPECTOR.CFG.tmp` file;
3. full read-back comparison of the temporary file;
4. rename to `MCINSPECTOR.CFG`;
5. mass-device sync.

A failed save leaves the current in-memory Settings unchanged.

## Settings currently persisted

- Display mode
- Filesystem test profile
- Preserve existing FREEMCB.CNF
- FMCB install read-back verification mode

Future settings should be added as new textual keys. Do not serialize the
`MciSettings` C structure directly; ABI-dependent binary config would recreate
the same versioning class that the recovery journal had to solve.
