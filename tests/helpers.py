"""
helpers.py — shared Win32 utilities for winzoo instrumented tests.

All Win32 calls go through ctypes so there are no required imports
beyond what's available from pywinauto and Pillow.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import json
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Optional, Tuple

import PIL.ImageGrab

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

WINZOO_EXE = Path(__file__).parent.parent / "build" / "Release" / "winzoo.exe"
WINZOO_SETTINGS_JSON = WINZOO_EXE.parent / "winzoo-settings.json"

# Known window class names (from C++ source)
CLASS_TASKBAR = "WinzooTaskbar"
CLASS_APPMENU = "WinzooAppMenu"
CLASS_POPUP   = "WinzooPopupMenu"
CLASS_SETTINGS_DIALOG = "#32770"
TITLE_SETTINGS_DIALOG = "Winzoo Settings"
CLASS_NATIVE_TASKBAR = "Shell_TrayWnd"

# Registry path for winzoo settings
REG_KEY = r"HKCU\Software\Winzoo"

user32  = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# ---------------------------------------------------------------------------
# Window finding
# ---------------------------------------------------------------------------

def find_window(class_name: str, title: Optional[str] = None) -> int:
    """Return the HWND of the first window matching class_name (and optional title)."""
    return user32.FindWindowW(class_name, title)


def find_window_ex(parent: int, class_name: str, title: Optional[str] = None) -> int:
    return user32.FindWindowExW(parent, None, class_name, title)


def wait_for_window(
    class_name: str,
    title: Optional[str] = None,
    timeout: float = 10.0,
    interval: float = 0.1,
) -> int:
    """Poll until the window appears. Returns HWND. Raises TimeoutError on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        hwnd = find_window(class_name, title)
        if hwnd and is_window_visible(hwnd):
            return hwnd
        time.sleep(interval)
    raise TimeoutError(
        f"Window class='{class_name}' title='{title}' not found within {timeout}s"
    )


def wait_for_window_gone(
    class_name: str,
    title: Optional[str] = None,
    timeout: float = 5.0,
    interval: float = 0.1,
) -> None:
    """Poll until the window disappears. Raises TimeoutError if still present."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        hwnd = find_window(class_name, title)
        if not hwnd or not is_window_visible(hwnd):
            return
        time.sleep(interval)
    raise TimeoutError(
        f"Window class='{class_name}' title='{title}' still present after {timeout}s"
    )


def is_window_visible(hwnd: int) -> bool:
    return bool(user32.IsWindowVisible(hwnd))


# ---------------------------------------------------------------------------
# Window geometry
# ---------------------------------------------------------------------------

Rect = Tuple[int, int, int, int]  # left, top, right, bottom


def get_window_rect(hwnd: int) -> Rect:
    rect = ctypes.wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)


def get_screen_size() -> Tuple[int, int]:
    """Return (width, height) of the primary monitor in pixels."""
    return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)


def get_window_dpi(hwnd: int) -> int:
    try:
        shcore = ctypes.windll.shcore
        dpi = ctypes.c_uint(0)
        # MDT_EFFECTIVE_DPI = 0
        shcore.GetDpiForMonitor(
            user32.MonitorFromWindow(hwnd, 2),  # MONITOR_DEFAULTTONEAREST
            0, ctypes.byref(dpi), ctypes.byref(ctypes.c_uint(0))
        )
        return dpi.value if dpi.value > 0 else 96
    except Exception:
        return 96


def window_center(hwnd: int) -> Tuple[int, int]:
    l, t, r, b = get_window_rect(hwnd)
    return (l + r) // 2, (t + b) // 2


# ---------------------------------------------------------------------------
# Mouse input
# ---------------------------------------------------------------------------

_INPUT_MOUSE = 0
_MOUSEEVENTF_MOVE        = 0x0001
_MOUSEEVENTF_LEFTDOWN    = 0x0002
_MOUSEEVENTF_LEFTUP      = 0x0004
_MOUSEEVENTF_RIGHTDOWN   = 0x0008
_MOUSEEVENTF_RIGHTUP     = 0x0010
_MOUSEEVENTF_ABSOLUTE    = 0x8000


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx",          ctypes.c_long),
        ("dy",          ctypes.c_long),
        ("mouseData",   ctypes.c_ulong),
        ("dwFlags",     ctypes.c_ulong),
        ("time",        ctypes.c_ulong),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUT(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = [("mi", _MOUSEINPUT)]
    _anonymous_ = ("_i",)
    _fields_ = [("type", ctypes.c_ulong), ("_i", _I)]


def _send_mouse(flags: int, x: int = 0, y: int = 0) -> None:
    sw, sh = get_screen_size()
    # Convert to normalized absolute coords (0–65535)
    nx = int(x * 65535 / (sw - 1)) if flags & _MOUSEEVENTF_ABSOLUTE else 0
    ny = int(y * 65535 / (sh - 1)) if flags & _MOUSEEVENTF_ABSOLUTE else 0
    inp = _INPUT(type=_INPUT_MOUSE)
    inp.mi.dx = nx
    inp.mi.dy = ny
    inp.mi.dwFlags = flags
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT))


def move_to(x: int, y: int) -> None:
    _send_mouse(_MOUSEEVENTF_MOVE | _MOUSEEVENTF_ABSOLUTE, x, y)
    time.sleep(0.05)


def click_at(x: int, y: int) -> None:
    """Left-click at absolute screen coordinates."""
    move_to(x, y)
    _send_mouse(_MOUSEEVENTF_LEFTDOWN | _MOUSEEVENTF_ABSOLUTE, x, y)
    time.sleep(0.05)
    _send_mouse(_MOUSEEVENTF_LEFTUP | _MOUSEEVENTF_ABSOLUTE, x, y)
    time.sleep(0.1)


def right_click_at(x: int, y: int) -> None:
    """Right-click at absolute screen coordinates."""
    move_to(x, y)
    _send_mouse(_MOUSEEVENTF_RIGHTDOWN | _MOUSEEVENTF_ABSOLUTE, x, y)
    time.sleep(0.05)
    _send_mouse(_MOUSEEVENTF_RIGHTUP | _MOUSEEVENTF_ABSOLUTE, x, y)
    time.sleep(0.1)


# ---------------------------------------------------------------------------
# Keyboard input (VK codes)
# ---------------------------------------------------------------------------

_KEYEVENTF_KEYUP = 0x0002

class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk",         ctypes.c_ushort),
        ("wScan",       ctypes.c_ushort),
        ("dwFlags",     ctypes.c_ulong),
        ("time",        ctypes.c_ulong),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUT_KEY(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT)]
    _anonymous_ = ("_i",)
    _fields_ = [("type", ctypes.c_ulong), ("_i", _I)]


_INPUT_KEYBOARD = 1
VK_ESCAPE = 0x1B


def press_key(vk: int) -> None:
    inp_down = _INPUT_KEY(type=_INPUT_KEYBOARD)
    inp_down.ki.wVk = vk
    inp_up = _INPUT_KEY(type=_INPUT_KEYBOARD)
    inp_up.ki.wVk = vk
    inp_up.ki.dwFlags = _KEYEVENTF_KEYUP
    user32.SendInput(1, ctypes.byref(inp_down), ctypes.sizeof(_INPUT_KEY))
    time.sleep(0.05)
    user32.SendInput(1, ctypes.byref(inp_up), ctypes.sizeof(_INPUT_KEY))
    time.sleep(0.1)


def press_escape() -> None:
    press_key(VK_ESCAPE)


# ---------------------------------------------------------------------------
# Screenshots
# ---------------------------------------------------------------------------

def screenshot_rect(left: int, top: int, right: int, bottom: int):
    """Capture and return a PIL Image of the given screen rectangle."""
    return PIL.ImageGrab.grab(bbox=(left, top, right, bottom))


def screenshot_window(hwnd: int):
    """Capture and return a PIL Image of the given window's bounding rect."""
    rect = get_window_rect(hwnd)
    return screenshot_rect(*rect)


# ---------------------------------------------------------------------------
# Popup dismissal
# ---------------------------------------------------------------------------

def dismiss_popups(wait: float = 0.2) -> None:
    """Press Escape repeatedly to dismiss any open winzoo popups."""
    for cls in (CLASS_APPMENU, CLASS_POPUP):
        hwnd = find_window(cls)
        if hwnd and is_window_visible(hwnd):
            press_escape()
            time.sleep(wait)
    # Dismiss settings dialog
    hwnd = find_window(CLASS_SETTINGS_DIALOG, TITLE_SETTINGS_DIALOG)
    if hwnd and is_window_visible(hwnd):
        press_escape()
        time.sleep(wait)


# ---------------------------------------------------------------------------
# Registry backup / restore
# ---------------------------------------------------------------------------

def backup_registry() -> Optional[str]:
    """
    Export HKCU\\Software\\Winzoo to a temp .reg file.
    Returns the temp file path, or None if the key doesn't exist yet.
    """
    tmp = tempfile.mktemp(suffix=".reg", prefix="winzoo_reg_backup_")
    result = subprocess.run(
        ["reg", "export", REG_KEY, tmp, "/y"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        return tmp
    return None  # key didn't exist


def restore_registry(backup_path: Optional[str]) -> None:
    """Import a previously backed-up .reg file, or delete the key if no backup."""
    import os
    if backup_path and Path(backup_path).exists():
        subprocess.run(["reg", "import", backup_path], capture_output=True)
        os.unlink(backup_path)
    else:
        # Key didn't exist before tests — delete it entirely
        subprocess.run(
            ["reg", "delete", REG_KEY, "/f"],
            capture_output=True
        )


# ---------------------------------------------------------------------------
# Settings seeding
# ---------------------------------------------------------------------------

TEST_SETTINGS = {
    "position": 0,              # Bottom
    "theme": 1,                 # Dark
    "thickness": 48,
    "pinnedExePaths": [],
    "showTrayIcons": True,
    "showClock": True,
    "showStatusZone": True,
    "taskbarMonitorMode": 0,    # AllMonitors
}


def seed_settings(extra: Optional[dict] = None) -> None:
    """
    Write a winzoo-settings.json next to the exe with known test defaults.
    Winzoo imports and deletes it on the next startup.
    """
    settings = dict(TEST_SETTINGS)
    if extra:
        settings.update(extra)
    WINZOO_SETTINGS_JSON.write_text(
        json.dumps(settings, indent=2), encoding="utf-8"
    )


# ---------------------------------------------------------------------------
# Native taskbar management
# ---------------------------------------------------------------------------

def hide_native_taskbar() -> None:
    hwnd = find_window(CLASS_NATIVE_TASKBAR)
    if hwnd:
        user32.ShowWindow(hwnd, 0)  # SW_HIDE


def show_native_taskbar() -> None:
    hwnd = find_window(CLASS_NATIVE_TASKBAR)
    if hwnd:
        user32.ShowWindow(hwnd, 5)  # SW_SHOW
    else:
        # If Shell_TrayWnd can't be found, explorer restart will recreate it
        subprocess.Popen(["explorer.exe"])


# ---------------------------------------------------------------------------
# Winzoo process helpers
# ---------------------------------------------------------------------------

def kill_existing_winzoo() -> None:
    """Kill any running winzoo instance gracefully, then force-kill if needed."""
    hwnd = find_window(CLASS_TASKBAR)
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        try:
            wait_for_window_gone(CLASS_TASKBAR, timeout=5)
        except TimeoutError:
            subprocess.run(["taskkill", "/F", "/IM", "winzoo.exe"], capture_output=True)
    show_native_taskbar()


def launch_winzoo() -> subprocess.Popen:
    """Launch the winzoo exe and return its Popen handle."""
    return subprocess.Popen([str(WINZOO_EXE)])


def wait_for_taskbar(timeout: float = 10.0) -> int:
    """Wait for WinzooTaskbar to become visible. Returns HWND."""
    return wait_for_window(CLASS_TASKBAR, timeout=timeout)
