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


def _env_names():
    text = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    return re.findall(r"^\[(env:[^\]]+)\]\s*$", text, re.M)


def _env_sets_lib_deps(name):
    return re.search(r"^lib_deps\s*=", _env_section(name), re.M) is not None


def _lvgl_pin(env=MAIN_ENV):
    pins = [d for d in _lib_deps(_env_section(env)) if d.startswith("lvgl/lvgl")]
    assert len(pins) == 1, f"[{env}] expected exactly one lvgl/lvgl entry, got {pins}"
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


def test_every_env_with_own_lib_deps_uses_the_same_pin():
    """Derived envs inherit lib_deps; one that overrides it must not drift from the main pin."""
    main_pin = _lvgl_pin()
    main_deps = _lib_deps(_env_section(MAIN_ENV))
    own = [n for n in _env_names() if _env_sets_lib_deps(n)]
    assert MAIN_ENV in own
    for name in own:
        assert _lvgl_pin(name) == main_pin, f"[{name}] lvgl pin differs from the main env"
        assert sorted(_lib_deps(_env_section(name))) == sorted(main_deps), f"[{name}] lib_deps differ from the main env"


def test_lv_conf_header_names_pinned_version():
    version = _lvgl_pin().split("@", 1)[1]
    assert version, "LVGL pin has no version"
    header = "\n".join(_conf_text().splitlines()[:10])
    assert f"v{version}" in header or f"LVGL {version}" in header


def test_lv_conf_critical_defines():
    text = _conf_text()
    defines = _active_defines(text)
    assert re.search(r"^#if 1\b", text, re.M), "lv_conf.h content guard must be '#if 1'"
    assert defines.get("LV_COLOR_FORMAT_DEFAULT") == "LV_COLOR_FORMAT_RGB565", "16-bit colour (LV_COLOR_DEPTH 16)"
    assert defines.get("LV_COLOR_16_SWAP") == "1", "byte swap depends on LV_COLOR_16_SWAP (display_manager.cpp)"
    assert defines.get("LV_USE_STDLIB_MALLOC") and defines["LV_USE_STDLIB_MALLOC"].startswith("LV_STDLIB_CUSTOM")


def test_lv_conf_assert_handler_reboots():
    defines = _active_defines(_conf_text())
    assert defines.get("LV_ASSERT_USE_CUSTOM_INCLUDE") == "1"
    assert "lv_assert_handler.h" in defines.get("LV_ASSERT_CUSTOM_INCLUDE", "")
    assert defines.get("LV_USE_ASSERT_NULL") == "1" and defines.get("LV_USE_ASSERT_MALLOC") == "1"
    handler = _active_defines((ROOT / "include" / "lv_assert_handler.h").read_text(encoding="utf-8"))
    assert handler.get("LV_ASSERT_HANDLER") == "abort();"
    assert "LV_ASSERT_HANDLER_INCLUDE" not in defines, "deprecated key; use LV_ASSERT_CUSTOM_INCLUDE"


def test_lv_conf_custom_fonts_declared():
    defines = _active_defines(_conf_text())
    assert defines.get("LV_FONT_USE_CUSTOM_INCLUDE") == "1"
    header = (ROOT / "include" / "lv_font_custom.h").read_text(encoding="utf-8")
    for size in (56, 60):
        assert f"LV_FONT_DECLARE(lv_font_montserrat_{size})" in header


def test_lv_conf_project_settings():
    defines = _active_defines(_conf_text())
    assert defines.get("LV_DEF_REFR_PERIOD") == "16"
    assert defines.get("LV_DPI_DEF") == "130"
    assert defines.get("LV_USE_LOG") == "1" and defines.get("LV_LOG_LEVEL") == "LV_LOG_LEVEL_ERROR"
    assert defines.get("LV_DRAW_SW_COMPLEX") == "1"
    assert defines.get("LV_USE_THEME_DEFAULT") == "1" and defines.get("LV_THEME_DEFAULT_DARK") == "0"
    assert defines.get("LV_FONT_DEFAULT") == "LV_FONT_DEFAULT_MONTSERRAT_14"
    assert defines.get("LV_COLOR_16_SWAP_DISABLE_WARNING") == "1"


def test_display_manager_guards_colour_swap():
    source = (ROOT / "src" / "hardware" / "display_manager.cpp").read_text(encoding="utf-8")
    assert re.search(r"#if !defined\(LV_COLOR_16_SWAP\) \|\| !LV_COLOR_16_SWAP\s*#error", source)
