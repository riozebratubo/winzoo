#!/usr/bin/env python3
"""
Checks Settings.h, SettingsFile.cpp, and SettingsDialog.cpp for coverage gaps:

  - All Settings struct fields exported to JSON file
  - All Settings struct fields imported from JSON file
  - All fields accessed in SettingsDialog.cpp exported to JSON file
  - All fields accessed in SettingsDialog.cpp imported from JSON file
  - Export/import key symmetry (exported-but-not-imported and vice-versa)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC  = ROOT / "src"

BOLD   = "\033[1m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
RED    = "\033[31m"
RESET  = "\033[0m"

# Disable ANSI if output is not a terminal (e.g. redirected to file)
if not sys.stdout.isatty():
    BOLD = GREEN = YELLOW = RED = RESET = ""


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def section(title: str) -> None:
    print(f"\n{BOLD}{title}{RESET}")
    print("-" * len(title))


def ok(msg: str) -> None:
    print(f"  {GREEN}OK  {RESET} {msg}")


def fail(msg: str, items: list) -> int:
    print(f"  {RED}FAIL{RESET} {msg} ({len(items)}):")
    for item in sorted(items):
        print(f"         - {item}")
    return 1


def warn(msg: str, items: list) -> None:
    print(f"  {YELLOW}WARN{RESET} {msg} ({len(items)}):")
    for item in sorted(items):
        print(f"         - {item}")


# ─── 1. Parse Settings struct field names ─────────────────────────────────────
settings_h = read(SRC / "Settings.h")
struct_match = re.search(r"struct Settings\s*\{(.*?)\};", settings_h, re.DOTALL)
if not struct_match:
    sys.exit("ERROR: Settings struct not found in Settings.h")

struct_body = struct_match.group(1)

# List every concrete type used in the struct so the regex stays precise.
KNOWN_TYPES = [
    r"bool",
    r"int",
    r"std::wstring",
    r"COLORREF",
    r"TaskbarPosition",
    r"ThemePreset",
    r"TaskbarMonitorMode",
    r"AppMenuLayout",
    r"AppMenuFlattenMode",
    r"MinimizedIndicatorType",
    r"std::vector<std::wstring>",
    r"std::map<std::wstring,\s*std::vector<std::wstring>>",
]
type_union = "(?:" + "|".join(KNOWN_TYPES) + ")"
all_fields: set[str] = set(re.findall(type_union + r"\s+(\w+)", struct_body))

# ─── 2. Parse SettingsFile.cpp keys ───────────────────────────────────────────
sf_cpp = read(SRC / "SettingsFile.cpp")

# Export keys — lambdas: wInt("key", ...), wBool, wStr, wUInt
export_keys: set[str] = set(re.findall(r'w(?:Int|Bool|Str|UInt)\("(\w+)"', sf_cpp))
# Manually-written array/object entries: j += "  \"keyName\": [" or {"
export_keys |= set(re.findall(r'j \+= "  \\"(\w+)\\"', sf_cpp))

# Import keys — helpers: ri("key",...), rb, rStr, rEnum
import_keys: set[str] = set(re.findall(r'r(?:i|b|Str|Enum)\("(\w+)"', sf_cpp))
# Special-case direct FindValue calls (arrays, COLORREF, etc.)
import_keys |= set(re.findall(r'FindValue\(content,\s*"(\w+)"\)', sf_cpp))

# ─── 3. Parse SettingsDialog.cpp — Settings fields accessed ───────────────────
dlg_cpp = read(SRC / "SettingsDialog.cpp")

dialog_fields: set[str] = set()
# Via pointer: data->settings->fieldName
dialog_fields |= set(re.findall(r"data->settings->(\w+)", dlg_cpp))
# Via local ref: const Settings& s = *data->settings; ... s.fieldName
# Intersect with known fields to avoid false matches on loop-variable "s".
dialog_fields |= set(re.findall(r"\bs\.(\w+)\b", dlg_cpp)) & all_fields

# ─── 4. Run checks ─────────────────────────────────────────────────────────────
issues = 0

section("Parsed counts")
print(f"  Settings.h  fields    : {len(all_fields)}")
print(f"  Export keys           : {len(export_keys)}")
print(f"  Import keys           : {len(import_keys)}")
print(f"  Dialog-accessed fields: {len(dialog_fields)}")

# ── A. Full Settings struct vs file ───────────────────────────────────────────
section("A — Settings struct vs SettingsFile.cpp")

missing_export = all_fields - export_keys
if missing_export:
    issues += fail("Fields in Settings.h NOT exported to file", list(missing_export))
else:
    ok("All Settings.h fields are exported")

missing_import = all_fields - import_keys
if missing_import:
    issues += fail("Fields in Settings.h NOT imported from file", list(missing_import))
else:
    ok("All Settings.h fields are imported")

# ── B. Export ↔ Import symmetry ───────────────────────────────────────────────
section("B — Export / Import symmetry")

exp_not_imp = export_keys - import_keys
imp_not_exp = import_keys - export_keys

if exp_not_imp:
    warn("Keys exported but NOT imported (value written to file, never read back)", list(exp_not_imp))
else:
    ok("Every exported key is also imported")

if imp_not_exp:
    warn("Keys imported but NOT exported (value read from file, never written)", list(imp_not_exp))
else:
    ok("Every imported key is also exported")

# ── C. Dialog fields vs file ───────────────────────────────────────────────────
section("C — SettingsDialog.cpp fields vs SettingsFile.cpp")

dlg_miss_export = dialog_fields - export_keys
dlg_miss_import = dialog_fields - import_keys

if dlg_miss_export:
    issues += fail("Dialog fields NOT exported to file", list(dlg_miss_export))
else:
    ok("All dialog-accessed fields are exported")

if dlg_miss_import:
    issues += fail("Dialog fields NOT imported from file", list(dlg_miss_import))
else:
    ok("All dialog-accessed fields are imported")

# ── D. Extra: fields in Settings.h not touched by dialog (informational) ──────
section("D — Settings.h fields NOT accessed by the dialog (informational)")
non_dialog = all_fields - dialog_fields
if non_dialog:
    print(f"  {YELLOW}INFO{RESET} {len(non_dialog)} field(s) exist in Settings.h but are not accessed "
          "in SettingsDialog.cpp\n       (managed elsewhere — no action needed unless this is surprising):")
    for f in sorted(non_dialog):
        print(f"         - {f}")
else:
    ok("All Settings.h fields are accessed in the dialog")

# ─── 5. Final verdict ─────────────────────────────────────────────────────────
section("Result")
if issues == 0:
    print(f"  {GREEN}{BOLD}PASS — no coverage gaps found.{RESET}")
else:
    print(f"  {RED}{BOLD}FAIL — {issues} issue(s) found (see FAIL lines above).{RESET}")

sys.exit(0 if issues == 0 else 1)
