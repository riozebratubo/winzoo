# winzoo

A lightweight taskbar replacement for Windows 10/11, written in C++20 using only Win32 APIs — no third-party libraries, no runtime dependencies beyond what Windows already ships.

When running, winzoo hides the native Windows taskbar and takes its place, reserving the same desktop space via the Windows AppBar API.

## Features

- Custom-drawn task buttons with app icon and title
- Custom-drawn apps menu
- Drag to reorder buttons
- Pin/unpin monitor-spaced
- Lots of configurations!
- Per monitor DPI aware (PerMonitorV2)
- Settings persisted in the registry (easily exportable/importable)
- Small excutable + dll, no installer

## Requirements to use

- Windows 10 or Windows 11

## Build

### Requirements

- Windows 10 or Windows 11
- Visual Studio 2026 (MSVC toolchain)
- CMake 3.20 or newer

### How to

Run `build.bat` from the project root. It configures and builds a Release binary in one step:

```bat
build.bat
```

The executable is written to `build\Release\winzoo.exe`.

To build manually:

```bat
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

> **Note:** The build uses CMake's Visual Studio generator, so no prior `vcvarsall.bat` call is needed — the generator handles the toolchain internally.

## Usage

Launch `winzoo.exe`. The native Windows taskbar is hidden automatically and restored when winzoo exits.

- **Left-click** a button to focus the window; left-click again to minimize it
- **Middle-click** a button to close the window (can be disabled in Settings)
- **Drag** a button left or right to reorder
- **Right-click** a button for per-app options
- **Right-click** the empty area at the end of the bar to open the main menu

To exit cleanly, right-click the taskbar → **Close Taskbar**. This restores the native Windows taskbar before quitting.
