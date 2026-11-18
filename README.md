# PowerGUI (PowerMenu)

A Windows power-button menu. Press the power button and a menu appears:

| Option | Mouse / touch | Keyboard |
|---|---|---|
| Lock | one click or tap | L |
| Sleep | hold 3 s | hold S |
| Power Off | hold 3 s | hold O |
| Restart | hold 3 s | hold R |

Releasing early, or sliding off an option, cancels it.

- Native Win32 C++, ARM64 and x64, no dependencies.
- Reads the power button's HID collections directly (Raw Input never delivers it).
- A LocalSystem service starts with Windows, opens the menu app at sign-in, and makes a press
  at the sign-in screen sleep the PC.
- The installer sets Windows' own power-button action to "Do nothing" and restores it on uninstall.

## Build

Needs Visual Studio 2026 (MSVC ARM64 + x64 tools) and NSIS 3.

```
build.cmd            exes + installers in dist\
build.cmd noinstall  exes only
```

`PowerMenu.exe --dry-run --show` opens the menu without running any action.
Tools in `tools\`: `hidprobe` (HID diagnostics), `touchtest` (touch/mouse injection for tests), `make-icon.ps1`.
