"""Guard for the pinned LVGL version and the lv_conf.h settings the project depends on.

Host only: reads platformio.ini and include/lv_conf.h as text. See docs/DESIGN-NOTES.md
(LVGL pin and config) for why each check exists.
"""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
MAIN_ENV = "env:waveshare-esp32s3-touch-amoled-164"
EXACT_VERSION = re.compile(r"^\d+\.\d+\.\d+$")


def _env_section(name):
    """Return the raw text of one [section] of platformio.ini."""
    text = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    match = re.search(r"^\[" + re.escape(name) + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    assert match, f"[{name}] not found in platformio.ini"
    return match.group(1)


def _lib_deps(section):
    """Active lib_deps entries (comments stripped, blanks dropped)."""
    match = re.search(r"^lib_deps\s*=(.*?)(?=^\S|\Z)", section, re.M | re.S)
    assert match, "lib_deps not found"
    deps = []
    for line in match.group(1).splitlines():
        line = re.split(r"\s[#;]|^\s*[#;]", line)[0].strip()
        if line:
            deps.append(line)
    return deps


def _lvgl_pin():
    pins = [d for d in _lib_deps(_env_section(MAIN_ENV)) if d.startswith("lvgl/lvgl")]
    assert len(pins) == 1, f"expected exactly one lvgl/lvgl entry, got {pins}"
    return pins[0]


def _conf_text():
    return (ROOT / "include" / "lv_conf.h").read_text(encoding="utf-8")


def _active_defines(text):
    """Map of non-commented `#define NAME value` lines (block and line comments removed)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    out = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]*([^\n]*)$", text, re.M):
        out[m.group(1)] = m.group(2).strip()
    return out


def test_lvgl_is_pinned_to_exact_version():
    pin = _lvgl_pin()
    assert pin.startswith("lvgl/lvgl@"), pin
    version = pin.split("@", 1)[1]
    assert EXACT_VERSION.match(version), f"LVGL must be pinned to X.Y.Z, got {pin!r}"


def test_flashz_is_pinned_to_exact_version():
    pins = [d for d in _lib_deps(_env_section(MAIN_ENV)) if "esp32-flashz" in d]
    assert len(pins) == 1, pins
    assert "@" in pins[0] and EXACT_VERSION.match(pins[0].split("@", 1)[1]), pins[0]


@pytest.mark.xfail(reason="lv_conf.h still says v9.3.0 until step 19.2 regenerates it", strict=False)
def test_lv_conf_header_names_pinned_version():
    version = _lvgl_pin().split("@", 1)[1]
    header = "\n".join(_conf_text().splitlines()[:10])
    assert f"v{version}" in header or f"LVGL {version}" in header


def test_lv_conf_critical_defines():
    text = _conf_text()
    defines = _active_defines(text)
    assert re.search(r"^#if 1\b", text, re.M), "lv_conf.h content guard must be '#if 1'"
    assert defines.get("LV_COLOR_DEPTH") == "16"
    assert defines.get("LV_COLOR_16_SWAP") == "1", "byte swap depends on LV_COLOR_16_SWAP (display_manager.cpp)"
    assert defines.get("LV_USE_STDLIB_MALLOC") and defines["LV_USE_STDLIB_MALLOC"].startswith("LV_STDLIB_CUSTOM")
