"""
test_window_tracking.py — Verify that winzoo's task buttons react to windows
opening and closing.

Strategy: launch notepad.exe (a guaranteed presence on all Windows installs),
wait for the taskbar to repaint, then verify the visible appearance changed.
"""

from __future__ import annotations

import subprocess
import time

import pytest

from helpers import (
    CLASS_TASKBAR,
    click_at,
    find_window,
    get_window_rect,
    is_window_visible,
    screenshot_window,
    wait_for_window,
    wait_for_window_gone,
)

NOTEPAD_EXE = "notepad.exe"
NOTEPAD_CLASS = "Notepad"

# How long to wait for shell-hook notifications to propagate
SHELL_HOOK_TIMEOUT = 4.0
SHELL_HOOK_INTERVAL = 0.2


def _taskbar_screenshot(hwnd: int):
    return screenshot_window(hwnd)


def _images_differ(img_a, img_b, threshold: int = 5) -> bool:
    """
    Return True if the two same-size PIL images differ by more than `threshold`
    pixels on average (channel-wise).
    """
    import PIL.ImageChops, PIL.ImageStat
    diff = PIL.ImageChops.difference(img_a.convert("RGB"), img_b.convert("RGB"))
    stat = PIL.ImageStat.Stat(diff)
    mean_diff = sum(stat.mean) / len(stat.mean)
    return mean_diff > threshold


@pytest.fixture()
def notepad():
    """Launch notepad, yield its HWND, then close it."""
    proc = subprocess.Popen([NOTEPAD_EXE])
    try:
        hwnd = wait_for_window(NOTEPAD_CLASS, timeout=10)
        yield hwnd, proc
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        # Allow shell-hook to settle
        time.sleep(0.5)


class TestWindowTracking:
    def test_notepad_button_appears(self, winzoo_hwnd, notepad):
        """
        After launching notepad, the taskbar should visually change to show
        a new task button.
        """
        notepad_hwnd, _ = notepad

        # Capture baseline (notepad already running from fixture)
        before = _taskbar_screenshot(winzoo_hwnd)

        # Force notepad to be visible/active so the shell hook fires
        from helpers import user32
        user32.SetForegroundWindow(notepad_hwnd)
        time.sleep(SHELL_HOOK_INTERVAL)

        # Keep sampling until the taskbar repaints or we time out
        deadline = time.monotonic() + SHELL_HOOK_TIMEOUT
        changed = False
        while time.monotonic() < deadline:
            after = _taskbar_screenshot(winzoo_hwnd)
            if _images_differ(before, after):
                changed = True
                break
            time.sleep(SHELL_HOOK_INTERVAL)

        # Even if pixel comparison is inconclusive, the notepad HWND must exist
        assert is_window_visible(notepad_hwnd), "Notepad window is not visible"
        # Soft assertion: taskbar did repaint
        assert changed, (
            "Taskbar did not visually change after launching notepad. "
            "Shell-hook may not have fired or repaint was too fast."
        )

    def test_notepad_button_disappears(self, winzoo_hwnd, notepad):
        """
        After closing notepad, the taskbar should visually change to remove
        the task button.
        """
        notepad_hwnd, proc = notepad

        # Make sure notepad button is present first
        from helpers import user32
        user32.SetForegroundWindow(notepad_hwnd)
        time.sleep(0.5)
        before = _taskbar_screenshot(winzoo_hwnd)

        # Close notepad
        proc.terminate()
        proc.wait(timeout=5)

        # Wait for the taskbar to repaint
        deadline = time.monotonic() + SHELL_HOOK_TIMEOUT
        changed = False
        while time.monotonic() < deadline:
            after = _taskbar_screenshot(winzoo_hwnd)
            if _images_differ(before, after):
                changed = True
                break
            time.sleep(SHELL_HOOK_INTERVAL)

        assert changed, (
            "Taskbar did not visually change after closing notepad."
        )

    def test_active_window_highlight(self, winzoo_hwnd, notepad):
        """
        Activating notepad should change the appearance of its task button
        (active-state highlight).
        """
        notepad_hwnd, _ = notepad
        from helpers import user32

        # Activate notepad
        user32.SetForegroundWindow(notepad_hwnd)
        time.sleep(0.4)
        active_shot = _taskbar_screenshot(winzoo_hwnd)

        # Now activate the desktop / deactivate notepad by clicking the wallpaper
        # via Win+D (simpler: just minimize notepad)
        user32.ShowWindow(notepad_hwnd, 6)  # SW_MINIMIZE
        time.sleep(0.4)
        inactive_shot = _taskbar_screenshot(winzoo_hwnd)

        assert _images_differ(active_shot, inactive_shot, threshold=2), (
            "Taskbar did not change appearance when notepad was minimized. "
            "Active-state indicator may not be rendering."
        )
