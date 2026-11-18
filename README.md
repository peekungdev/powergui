# PowerGUI

**A safer power button for Windows.** Press the power button and, instead of the PC going straight
to sleep, a menu asks what you want to do. Anything that turns the PC off has to be held for
3 seconds, so a stray press never costs you your work.

## What you get

| Option | Mouse / touch | Keyboard |
|---|---|---|
| **Lock** | one click or tap | `L` |
| **Sleep** | hold 3 seconds | hold `S` |
| **Power Off** | hold 3 seconds | hold `O` |
| **Restart** | hold 3 seconds | hold `R` |

- A ring fills while you hold; let go early, or slide off the option, and nothing happens.
- `Esc`, **Cancel**, clicking outside, or pressing the power button again closes the menu.
- Starts with Windows. At the sign-in screen, before anyone signs in, the power button sleeps
  the PC as usual.
- The press that wakes the PC is ignored, so waking it never opens the menu.
- Works with the laptop's power button and with power keys on keyboards.
- Native ARM64 and x64 builds, no runtimes to install.

## Install

Download the installer for your PC from
[Releases](https://github.com/peekungdev/powergui/releases):

- `PowerMenu-<version>-arm64-setup.exe` for ARM64 Windows (Snapdragon and similar)
- `PowerMenu-<version>-x64-setup.exe` for Intel / AMD Windows

The installers are unsigned, so Windows SmartScreen will ask you to confirm.

Setup sets Windows' own power-button action to **Do nothing** (otherwise Windows would act
before the menu appears). Uninstalling from **Settings > Apps** puts your original setting back.

## How it works

- Windows never delivers the power button through Raw Input, so PowerMenu reads the button's
  HID collections directly; Windows opens them shared, so this takes nothing away from it.
- A small LocalSystem service runs from startup. It opens the menu app when you sign in, and
  handles the button itself when no one is signed in.
- The menu app runs in your session and shows the menu, using Windows' normal Lock, Sleep,
  Shut down and Restart calls.

Logs: `%LOCALAPPDATA%\PowerMenu\PowerMenu.log` (menu app) and
`C:\ProgramData\PowerMenu\service.log` (service).

## Build from source

Needs Visual Studio 2026 with the MSVC ARM64 and x64 tools, and NSIS 3.

```
build.cmd            executables and both installers (in dist\)
build.cmd noinstall  executables only
```

To try the menu without anything actually happening:

```
PowerMenu.exe --dry-run --show
```

Other command-line switches: `--show`, `--quit`, `--install-service`, `--uninstall-service`,
`--take-button`, `--restore-button`.

Tools in `tools\`: `hidprobe` (lists HID collections and records button reports),
`touchtest` (injects real touch or mouse input for testing), `make-icon.ps1`.

## Known limits

- The menu can't appear over the lock screen; a press there puts the PC to sleep.
- Holding the hardware button for about 10 seconds still forces the PC off; that is done by the
  hardware, not Windows.
