"""
test_interactions.py — Verify mouse-driven interactions on the winzoo taskbar.

All clicks use absolute screen coordinates derived from the taskbar's window rect.
"""

from __future__ import annotations

import time

import pytest

from helpers import (
    CLASS_APPMENU,
    CLASS_POPUP,
    CLASS_SETTINGS_DIALOG,
    TITLE_SETTINGS_DIALOG,
    click_at,
    find_window,
    get_window_rect,
    is_window_visible,
    press_escape,
    right_click_at,
    wait_for_window,
    wait_for_window_gone,
    user32,
)

# How long to wait for popup windows after a click
POPUP_TIMEOUT = 4.0

# ───────────────────────────────────────────────────────────
# Helper: calculate click targets from the taskbar rect
# ───────────────────────────────────────────────────────────

def _taskbar_start_btn(hwnd: int):
    """
    The Start button is the leftmost element in the primary taskbar.
    We click ~24px from the left edge, vertically centred.
    """
    l, t, r, b = get_window_rect(hwnd)
    cx = l + 24
    cy = (t + b) // 2
    return cx, cy


def _taskbar_background(hwnd: int):
    """
    A point in the middle of the taskbar that should be taskbar background
    (no buttons if no apps are running). We use 60 % of the width, centred.
    """
    l, t, r, b = get_window_rect(hwnd)
    cx = l + int((r - l) * 0.6)
    cy = (t + b) // 2
    return cx, cy


# ───────────────────────────────────────────────────────────
# Tests
# ───────────────────────────────────────────────────────────

class TestRightClickMenu:
    def test_rightclick_menu_opens(self, winzoo_hwnd):
        """Right-clicking the taskbar background should open WinzooPopupMenu."""
        x, y = _taskbar_background(winzoo_hwnd)
        right_click_at(x, y)
        hwnd = wait_for_window(CLASS_POPUP, timeout=POPUP_TIMEOUT)
        assert hwnd and is_window_visible(hwnd), \
            "WinzooPopupMenu did not appear after right-click"

    def test_rightclick_menu_dismisses_on_escape(self, winzoo_hwnd):
        """Pressing Escape should close the popup menu."""
        x, y = _taskbar_background(winzoo_hwnd)
        right_click_at(x, y)
        wait_for_window(CLASS_POPUP, timeout=POPUP_TIMEOUT)

        press_escape()
        wait_for_window_gone(CLASS_POPUP, timeout=3.0)
        hwnd = find_window(CLASS_POPUP)
        assert not hwnd or not is_window_visible(hwnd), \
            "WinzooPopupMenu still visible after Escape"


class TestAppMenu:
    def test_appmenu_opens(self, winzoo_hwnd):
        """Clicking the Start button area should open WinzooAppMenu."""
        x, y = _taskbar_start_btn(winzoo_hwnd)
        click_at(x, y)
        hwnd = wait_for_window(CLASS_APPMENU, timeout=POPUP_TIMEOUT)
        assert hwnd and is_window_visible(hwnd), \
            "WinzooAppMenu did not appear after clicking Start button"

    def test_appmenu_dismisses_on_escape(self, winzoo_hwnd):
        """Pressing Escape should close the app menu."""
        x, y = _taskbar_start_btn(winzoo_hwnd)
        click_at(x, y)
        wait_for_window(CLASS_APPMENU, timeout=POPUP_TIMEOUT)

        press_escape()
        wait_for_window_gone(CLASS_APPMENU, timeout=3.0)
        hwnd = find_window(CLASS_APPMENU)
        assert not hwnd or not is_window_visible(hwnd), \
            "WinzooAppMenu still visible after Escape"

    def test_appmenu_closes_on_second_click(self, winzoo_hwnd):
        """Clicking the Start button while the menu is open should close it."""
        x, y = _taskbar_start_btn(winzoo_hwnd)

        click_at(x, y)
        wait_for_window(CLASS_APPMENU, timeout=POPUP_TIMEOUT)

        # Second click on the same button
        click_at(x, y)
        wait_for_window_gone(CLASS_APPMENU, timeout=3.0)
        hwnd = find_window(CLASS_APPMENU)
        assert not hwnd or not is_window_visible(hwnd), \
            "WinzooAppMenu still visible after second Start button click"


class TestSettingsDialog:
    def _open_settings(self, winzoo_hwnd):
        """Right-click → find 'Settings' item in WinzooPopupMenu → click it."""
        x, y = _taskbar_background(winzoo_hwnd)
        right_click_at(x, y)
        popup_hwnd = wait_for_window(CLASS_POPUP, timeout=POPUP_TIMEOUT)
        assert popup_hwnd, "Popup menu did not open"

        # The Settings menu item is inside WinzooPopupMenu.
        # We click at the popup's horizontal centre, ~2/3 down its height
        # (Settings is typically near the bottom of the menu).
        pl, pt, pr, pb = get_window_rect(popup_hwnd)
        # Try clicking at several vertical positions to find "Settings"
        menu_h = pb - pt
        for frac in (0.75, 0.60, 0.85, 0.50):
            click_at((pl + pr) // 2, pt + int(menu_h * frac))
            dlg = find_window(CLASS_SETTINGS_DIALOG, TITLE_SETTINGS_DIALOG)
            if dlg and is_window_visible(dlg):
                return dlg
            time.sleep(0.3)

        return find_window(CLASS_SETTINGS_DIALOG, TITLE_SETTINGS_DIALOG)

    def test_settings_dialog_opens(self, winzoo_hwnd):
        """Right-click → Settings → Winzoo Settings dialog should appear."""
        dlg = self._open_settings(winzoo_hwnd)
        assert dlg and is_window_visible(dlg), \
            "Winzoo Settings dialog did not open"

    def test_settings_dialog_closes_on_escape(self, winzoo_hwnd):
        """The Settings dialog should close when Escape is pressed."""
        dlg = self._open_settings(winzoo_hwnd)
        if not dlg:
            pytest.skip("Could not open Settings dialog — skipping close test")

        press_escape()
        wait_for_window_gone(
            CLASS_SETTINGS_DIALOG, TITLE_SETTINGS_DIALOG, timeout=3.0
        )
        hwnd = find_window(CLASS_SETTINGS_DIALOG, TITLE_SETTINGS_DIALOG)
        assert not hwnd or not is_window_visible(hwnd), \
            "Settings dialog still visible after Escape"
