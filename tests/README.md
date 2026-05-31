# winzoo instrumented tests

Python end-to-end tests that launch the real winzoo binary and verify its behaviour
by interacting with it as a user would — mouse clicks, keyboard input, and screenshot
comparison.

## Requirements

- Windows 10/11 with an interactive desktop session (not headless)
- Python 3.8+
- `build\Release\winzoo.exe` must exist — run `build.bat` first
- `gh` CLI not required for tests

## Setup

```powershell
cd D:\dev\winzoo
pip install -r tests\requirements.txt
```

## Running

```powershell
# All tests
pytest tests\ -v

# A single file
pytest tests\test_startup.py -v

# A single test
pytest tests\test_interactions.py::TestAppMenu::test_appmenu_opens -v
```

> **Note:** winzoo will be launched and shut down automatically by the session
> fixture. Any running winzoo instance will be killed before the test run and the
> native Windows taskbar will be restored afterwards.

## Visual baseline tests

The first time you run visual tests (or after a UI change), generate baselines:

```powershell
pytest tests\test_visual.py --update-baselines -v
```

This saves PNG files into `tests\screenshots\`. Commit them so CI comparisons work.

On subsequent runs, omit `--update-baselines` and the tests will compare against
the stored images (mean pixel difference ≤ 15 by default).

## File overview

| File | Purpose |
|---|---|
| `helpers.py` | Win32 utilities: window finding, mouse input, screenshots, registry backup |
| `conftest.py` | Session + function pytest fixtures |
| `test_startup.py` | Window class exists, position, size |
| `test_window_tracking.py` | Task button appears/disappears with apps |
| `test_interactions.py` | Right-click menu, app menu, settings dialog |
| `test_visual.py` | Screenshot baseline comparison |
| `screenshots/` | Baseline PNG images (committed) |

## Troubleshooting

- **`winzoo.exe not found`** — run `build.bat` first
- **`gh auth status` fails** — unrelated to tests; `gh` is only needed for `release.py`
- **Tests hang** — a stray modal dialog may be blocking; press Escape or close it manually
- **Visual tests always fail** — regenerate baselines with `--update-baselines` (DPI or
  theme differences between machines can change pixel values)
- **`WinzooSingleInstance` mutex error** — kill the running winzoo.exe via Task Manager,
  then re-run
