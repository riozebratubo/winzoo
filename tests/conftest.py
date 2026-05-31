"""
conftest.py — pytest fixtures for winzoo instrumented tests.
"""

from __future__ import annotations

import time
import pytest

from helpers import (
    backup_registry,
    dismiss_popups,
    kill_existing_winzoo,
    launch_winzoo,
    restore_registry,
    seed_settings,
    show_native_taskbar,
    wait_for_taskbar,
    WINZOO_EXE,
)


def pytest_addoption(parser):
    parser.addoption(
        "--update-baselines",
        action="store_true",
        default=False,
        help="Regenerate screenshot baseline images instead of comparing them.",
    )


@pytest.fixture(scope="session")
def update_baselines(request) -> bool:
    return request.config.getoption("--update-baselines")


@pytest.fixture(scope="session")
def winzoo_hwnd():
    """
    Session-scoped fixture that:
      1. Verifies the binary exists
      2. Backs up HKCU\\Software\\Winzoo
      3. Seeds a deterministic settings JSON
      4. Kills any running winzoo instance
      5. Launches a fresh winzoo
      6. Waits for the taskbar window to appear
      7. Yields the HWND
      8. Tears down: kills winzoo, restores native taskbar, restores registry
    """
    assert WINZOO_EXE.exists(), (
        f"winzoo.exe not found at {WINZOO_EXE}.\n"
        "Run build.bat first, then re-run the tests."
    )

    reg_backup = backup_registry()

    seed_settings()
    kill_existing_winzoo()
    time.sleep(0.5)

    proc = launch_winzoo()
    try:
        hwnd = wait_for_taskbar(timeout=15)
        # Give it a moment to fully initialise tray/status icons
        time.sleep(1.0)
        yield hwnd
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        show_native_taskbar()
        restore_registry(reg_backup)


@pytest.fixture(autouse=True)
def clean_state(winzoo_hwnd):
    """
    Function-scoped autouse fixture.
    Dismisses any stray popups before and after every test.
    """
    dismiss_popups()
    yield
    dismiss_popups()
