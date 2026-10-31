# Eglantine Hub

`eglantine-hub.exe` sits next to `eglantine.exe` in the Windows zip. It is a game library and
launcher that adds what the emulator's own game list does not do.

## What it does

- **Library of everything bootable**: NSP, NSZ, XCI, XCZ, NRO homebrew and unpacked ExeFS games
  (a folder with `main` + `main.npdm`, as used by Switch 2 dumps). Add folders, or drag them onto
  the window. Subfolders are scanned; Eglantine's `nand` folder is skipped.
- **No keys needed to list games**: title IDs come from the NSP's ticket name, the
  `[0100…]` tag in the file name, or `main.npdm`. Updates and DLC are recognised and hidden by
  default (tick *Updates & DLC* to see them).
- **File check**: every file's container header is read and its expected size compared with the
  real one, so a download that never finished shows a ⚠ instead of crashing at boot.
- **Tuned launch**, per game or as a default:
  - *Never power-throttle*: opts Eglantine out of Windows EcoQoS so it keeps full clocks when
    unfocused. On by default.
  - *High process priority*: Windows schedules Eglantine before ordinary apps.
  - *Performance cores only*: on 12th-gen and newer Intel CPUs, keeps Eglantine on the P-cores.
    Can smooth frame pacing; can also slow first-time shader compilation, so try it per game.
  - Fullscreen, profile (`-u`) and any extra command-line arguments.

  The process is created suspended, tuned, then resumed, so the settings apply from the start.
- **Favourites, recently played, time played via the Hub.**
- **Cover art**: set any image, or drop `<game file name>.png`, `cover.png` or `icon.png` next to
  the game.
- **Clear shader cache** for one game, for glitches after driver or emulator updates.

## Where it keeps its data

If a `user` folder exists next to `eglantine-hub.exe` (Eglantine's portable mode), in
`user/hub/`. Otherwise in `%APPDATA%/Eglantine/EglantineHub/hub.json`.

## Building

It is built with the emulator when `ENABLE_QT` is on; turn it off with `-DEGLANTINE_HUB=OFF`.
The target is `eglantine-hub`. `src/eglantine_hub/hub_core.*` has no Qt dependency.
