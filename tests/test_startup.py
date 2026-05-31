"""
test_startup.py — Verify that winzoo launches correctly.
"""

from __future__ import annotations

import subprocess
import time

import pytest

from helpers import (
    CLASS_TASKBAR,
    WINZOO_EXE,
    find_window,
    get_screen_size,
    get_window_dpi,
    get_window_rect,
    is_window_visible,
    kill_existing_winzoo,
    launch_winzoo,
    seed_settings,
    wait_for_taskbar,
    wait_for_window_gone,
)


class TestStartup:
    def test_window_exists(self, winzoo_hwnd):
        """WinzooTaskbar HWND must be non-null."""
        assert winzoo_hwnd != 0, "WinzooTaskbar window not found"

    def test_window_visible(self, winzoo_hwnd):
        """WinzooTaskbar must be visible on screen."""
        assert is_window_visible(winzoo_hwnd), "WinzooTaskbar is not visible"

    def test_window_at_bottom_edge(self, winzoo_hwnd):
        """
        With default settings (position=Bottom), the taskbar's bottom edge
        must align with the primary monitor's bottom edge.
        """
        l, t, r, b = get_window_rect(winzoo_hwnd)
        sw, sh = get_screen_size()

        assert b == sh, (
            f"Taskbar bottom ({b}) should equal screen height ({sh})"
        )
        assert l == 0, f"Taskbar left ({l}) should be 0"
        assert r == sw, (
            f"Taskbar right ({r}) should equal screen width ({sw})"
        )

    def test_window_spans_screen_width(self, winzoo_hwnd):
        """Taskbar must span the full width of the primary monitor."""
        l, t, r, b = get_window_rect(winzoo_hwnd)
        sw, _ = get_screen_size()
        width = r - l
        assert width == sw, (
            f"Taskbar width ({width}) should equal screen width ({sw})"
        )

    def test_window_thickness(self, winzoo_hwnd):
        """
        Default thickness is 48 logical px. At 96 DPI that is exactly 48 physical px;
        at 125 % scaling it would be 60 px. We accept any value in [40, 100].
        """
        l, t, r, b = get_window_rect(winzoo_hwnd)
        height = b - t
        assert 40 <= height <= 100, (
            f"Taskbar height ({height}px) is outside expected range [40, 100]"
        )

    def test_no_duplicate_instance(self, winzoo_hwnd):
        """
        Launching a second copy while winzoo is running should fail immediately
        due to the WinzooSingleInstance mutex.
        """
        proc = launch_winzoo()
        time.sleep(1.5)
        # The second process should have exited quickly
        ret = proc.poll()
        if ret is None:
            proc.terminate()
            proc.wait(timeout=3)
        # Second instance should either have exited or there should still be
        # exactly one WinzooTaskbar window
        assert find_window(CLASS_TASKBAR) == winzoo_hwnd, (
            "A second WinzooTaskbar appeared — single-instance guard failed"
        )
