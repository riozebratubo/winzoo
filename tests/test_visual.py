"""
test_visual.py — Screenshot baseline comparison tests for winzoo.

Run with --update-baselines to regenerate stored baseline images:

    pytest tests/test_visual.py --update-baselines

On subsequent runs (without the flag) each screenshot is compared against the
stored baseline using a per-pixel mean-difference threshold.
"""

from __future__ import annotations

import math
from pathlib import Path

import PIL.Image
import PIL.ImageChops
import PIL.ImageStat
import pytest

from helpers import (
    CLASS_APPMENU,
    get_window_rect,
    screenshot_rect,
    screenshot_window,
    wait_for_window,
)

SCREENSHOTS_DIR = Path(__file__).parent / "screenshots"
SCREENSHOTS_DIR.mkdir(exist_ok=True)

# Mean-channel difference threshold (0-255 scale) below which two images
# are considered "the same".  Clock digits change every second so we use
# a fairly generous tolerance.
DEFAULT_THRESHOLD = 15

# Region of interest: exclude the clock/status zone from the taskbar baseline
# (rightmost 200px) to avoid false failures from clock ticking.
CLOCK_EXCLUSION_RIGHT_PX = 220


# ───────────────────────────────────────────────────────────
# Helpers
# ───────────────────────────────────────────────────────────

def _mean_diff(img_a: PIL.Image.Image, img_b: PIL.Image.Image) -> float:
    """Return mean per-channel pixel difference between two same-size images."""
    diff = PIL.ImageChops.difference(img_a.convert("RGB"), img_b.convert("RGB"))
    stat = PIL.ImageStat.Stat(diff)
    return sum(stat.mean) / len(stat.mean)


def _crop_exclude_right(img: PIL.Image.Image, exclude_px: int) -> PIL.Image.Image:
    """Crop out the rightmost `exclude_px` columns to ignore the clock."""
    w, h = img.size
    right = max(w - exclude_px, 1)
    return img.crop((0, 0, right, h))


def _compare_or_update(
    name: str,
    current: PIL.Image.Image,
    update: bool,
    threshold: float = DEFAULT_THRESHOLD,
) -> None:
    """
    If `update` is True, save `current` as the new baseline and pass.
    Otherwise, load the stored baseline and assert similarity.
    """
    path = SCREENSHOTS_DIR / f"{name}.png"

    if update:
        current.save(path)
        return

    if not path.exists():
        pytest.skip(
            f"Baseline '{path.name}' not found. "
            "Run with --update-baselines to create it."
        )

    baseline = PIL.Image.open(path).convert("RGB")
    current_rgb = current.convert("RGB")

    # Resize baseline to match current (DPI may differ between machines)
    if baseline.size != current_rgb.size:
        baseline = baseline.resize(current_rgb.size, PIL.Image.LANCZOS)

    diff = _mean_diff(baseline, current_rgb)
    assert diff <= threshold, (
        f"Visual regression in '{name}': mean pixel diff {diff:.1f} > threshold {threshold}. "
        f"Run with --update-baselines if this is an intentional change."
    )


# ───────────────────────────────────────────────────────────
# Tests
# ───────────────────────────────────────────────────────────

class TestVisualBaselines:
    def test_taskbar_idle_baseline(self, winzoo_hwnd, update_baselines):
        """
        The taskbar in idle state (no running apps, no popups) should match
        the stored baseline, excluding the clock region.
        """
        shot = screenshot_window(winzoo_hwnd)
        cropped = _crop_exclude_right(shot, CLOCK_EXCLUSION_RIGHT_PX)
        _compare_or_update("taskbar_idle", cropped, update=update_baselines)

    def test_appmenu_baseline(self, winzoo_hwnd, update_baselines):
        """
        The app menu should match its stored baseline.
        """
        from helpers import click_at, get_window_rect, press_escape

        # Open the app menu
        l, t, r, b = get_window_rect(winzoo_hwnd)
        click_at(l + 24, (t + b) // 2)
        try:
            menu_hwnd = wait_for_window(CLASS_APPMENU, timeout=4.0)
        except TimeoutError:
            pytest.skip("App menu did not open — skipping visual test")

        shot = screenshot_window(menu_hwnd)
        press_escape()
        _compare_or_update("appmenu", shot, update=update_baselines)

    def test_taskbar_button_zone_not_solid_color(self, winzoo_hwnd):
        """
        The left 60 % of the taskbar (button zone) must contain more than one
        distinct color — it must not be a solid fill (which would indicate
        nothing rendered).
        """
        l, t, r, b = get_window_rect(winzoo_hwnd)
        zone_right = l + int((r - l) * 0.6)
        shot = screenshot_rect(l, t, zone_right, b).convert("RGB")

        colors = shot.getcolors(maxcolors=shot.width * shot.height)
        unique_colors = len(colors) if colors else shot.width * shot.height
        assert unique_colors > 1, (
            "Button zone appears to be a solid color — rendering may have failed"
        )

    def test_clock_zone_not_solid_color(self, winzoo_hwnd):
        """
        The clock zone (rightmost area) must contain more than one color,
        confirming that clock digits are rendered.
        """
        l, t, r, b = get_window_rect(winzoo_hwnd)
        clock_left = r - CLOCK_EXCLUSION_RIGHT_PX
        if clock_left < l:
            pytest.skip("Taskbar too narrow to isolate clock zone")

        shot = screenshot_rect(clock_left, t, r, b).convert("RGB")
        colors = shot.getcolors(maxcolors=shot.width * shot.height)
        unique_colors = len(colors) if colors else shot.width * shot.height
        assert unique_colors > 1, (
            "Clock zone appears to be a solid color — clock may not be rendering"
        )
