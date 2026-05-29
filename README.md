# winzoo

A lightweight taskbar replacement for Windows 10/11, written in C++20 using only Win32 APIs — no third-party libraries, no runtime dependencies beyond what Windows already ships.

When running, winzoo hides the native Windows taskbar and takes its place, reserving the same desktop space via the Windows AppBar API.

## Features

- Custom-drawn task buttons with app icon and title
- Drag to reorder buttons
- Right-click context menu on each button: open new window, pin/unpin, close
- Right-click on the taskbar itself: settings, about, close
- Middle-click to close a window (configurable)
- 5 built-in color themes: Dark, Light, Accent, Forest, Sunset
- Taskbar position: top, bottom, left, right, or floating
- Configurable bar thickness
- PerMonitorV2 DPI aware
- Settings persisted in the registry under `HKCU\Software\Winzoo`
- Single 60 KB executable, no installer

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2026 (MSVC toolchain)
- CMake 3.20 or newer

## Build

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

## Settings

Open Settings via right-click → **Settings**.

| Option | Description |
|--------|-------------|
| Position | Top / Bottom / Left / Right / Floating |
| Theme | Dark / Light / Accent / Forest / Sunset |
| Bar thickness | Height (horizontal) or width (vertical) in pixels at 96 DPI |
| Middle-click closes window | Send WM\_CLOSE to the window under the cursor on middle-click |
