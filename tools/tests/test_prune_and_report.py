"""Tests for step 7.1: post_build.prune_cache and grind_report session_id handling.

post_build.py is loaded by path with a stubbed SCons Import()/env. grind_report.py needs
streamlit/pandas/plotly/scipy at import time, so load_session_details is extracted from the
source via ast and executed with a minimal pandas stand-in against a temp sqlite DB.
"""

import ast
import os
import sqlite3
import types
from pathlib import Path
from unittest import mock

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent


def _load_post_build():
    path = TOOLS_DIR / "build-scripts" / "post_build.py"
    env = mock.MagicMock()
    ns = {"__name__": "post_build_under_test", "__file__": str(path),
          "Import": lambda *a, **k: None, "env": env}
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), ns)
    return types.SimpleNamespace(**ns)


pb = _load_post_build()


def _make(dirpath, names):
    for n in names:
        (dirpath / n).write_bytes(b"x")


def _left(dirpath):
    return sorted(os.listdir(dirpath))


def _names(paths):
    return sorted(os.path.basename(p) for p in paths)


def test_more_than_keep_removes_oldest(tmp_path):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 26)])
    removed = pb.prune_cache(str(tmp_path), keep=20)
    assert _names(removed) == [f"build_{i:03d}.bin" for i in range(1, 6)]
    assert len(_left(tmp_path)) == 20
    assert "build_006.bin" in _left(tmp_path)


def test_default_keep_is_20():
    assert pb.FIRMWARE_CACHE_KEEP == 20


def test_fewer_than_keep_removes_nothing(tmp_path):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 6)])
    assert pb.prune_cache(str(tmp_path), keep=20) == []
    assert len(_left(tmp_path)) == 5


def test_exactly_keep_removes_nothing(tmp_path):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 21)])
    assert pb.prune_cache(str(tmp_path), keep=20) == []


def test_non_matching_names_untouched(tmp_path):
    others = ["README.md", "README", "build_abc.bin", "build_.bin", "build_12.bin.bak",
              "xbuild_001.bin", "build_001.bin.tmp", "firmware.bin", "build_1a.bin", "build_-5.bin"]
    _make(tmp_path, others)
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 6)])
    removed = pb.prune_cache(str(tmp_path), keep=2)
    assert len(removed) == 3
    for n in others:
        assert n in _left(tmp_path)


def test_subdirectory_not_followed(tmp_path):
    (tmp_path / "sub").mkdir()
    _make(tmp_path / "sub", [f"build_{i:03d}.bin" for i in range(1, 6)])
    _make(tmp_path, ["build_001.bin", "build_002.bin"])
    pb.prune_cache(str(tmp_path), keep=1)
    assert len(os.listdir(tmp_path / "sub")) == 5


def test_four_digit_numbers_sort_numerically(tmp_path):
    _make(tmp_path, ["build_999.bin", "build_1000.bin", "build_1001.bin", "build_099.bin"])
    removed = pb.prune_cache(str(tmp_path), keep=2)
    assert _names(removed) == ["build_099.bin", "build_999.bin"]
    assert _left(tmp_path) == ["build_1000.bin", "build_1001.bin"]


def test_protected_oldest_survives(tmp_path):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 26)])
    protect = tmp_path / "build_001.bin"
    removed = pb.prune_cache(str(tmp_path), keep=20, protect=str(protect))
    assert protect.exists()
    assert str(protect) not in removed
    assert len(removed) == 4
    assert len(_left(tmp_path)) == 21


def test_other_env_dirs_untouched(tmp_path):
    env_a, env_b = tmp_path / "a", tmp_path / "b"
    env_a.mkdir()
    env_b.mkdir()
    _make(env_a, [f"build_{i:03d}.bin" for i in range(1, 11)])
    _make(env_b, [f"build_{i:03d}.bin" for i in range(1, 11)])
    pb.prune_cache(str(env_a), keep=3)
    assert len(os.listdir(env_a)) == 3
    assert len(os.listdir(env_b)) == 10


def test_returns_removed_paths_and_they_are_gone(tmp_path):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 6)])
    removed = pb.prune_cache(str(tmp_path), keep=3)
    assert len(removed) == 2
    for p in removed:
        assert os.path.dirname(p) == str(tmp_path)
        assert not os.path.exists(p)


def test_empty_dir(tmp_path):
    assert pb.prune_cache(str(tmp_path), keep=20) == []


def test_deletion_error_does_not_raise(tmp_path, capsys):
    _make(tmp_path, [f"build_{i:03d}.bin" for i in range(1, 6)])
    real_remove = os.remove

    def flaky(path):
        if path.endswith("build_001.bin"):
            raise PermissionError("locked")
        real_remove(path)

    with mock.patch.object(pb.os, "remove", side_effect=flaky):
        removed = pb.prune_cache(str(tmp_path), keep=2)
    assert _names(removed) == ["build_002.bin", "build_003.bin"]
    assert (tmp_path / "build_001.bin").exists()
    assert "could not prune" in capsys.readouterr().out


# ---------------------------------------------------------------- grind_report

class _Frame:
    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows

    # phase_name cleanup is only applied when the column exists; tests use it on events
    def __getitem__(self, key):
        return types.SimpleNamespace(str=types.SimpleNamespace(strip=lambda: key))

    def __setitem__(self, key, value):
        pass


def _fake_pd():
    def read_sql_query(sql, conn, params=()):
        cur = conn.execute(sql, params)
        return _Frame([d[0] for d in cur.description], cur.fetchall())
    return types.SimpleNamespace(read_sql_query=read_sql_query)


def _load_details_func(db_file):
    src = (TOOLS_DIR / "streamlit-reports" / "grind_report.py").read_text(encoding="utf-8")
    tree = ast.parse(src)
    func = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "load_session_details")
    func.decorator_list = []
    module = ast.Module(body=[func], type_ignores=[])
    ns = {"sqlite3": sqlite3, "pd": _fake_pd(), "DB_FILE": str(db_file)}
    exec(compile(ast.fix_missing_locations(module), "grind_report.py", "exec"), ns)
    return ns["load_session_details"]


@pytest.fixture
def db(tmp_path):
    path = tmp_path / "t.db"
    conn = sqlite3.connect(path)
    conn.execute("CREATE TABLE grind_events (session_id INTEGER, timestamp_ms INTEGER, phase_name TEXT)")
    conn.execute("CREATE TABLE grind_measurements (session_id INTEGER, timestamp_ms INTEGER, weight REAL)")
    conn.executemany("INSERT INTO grind_events VALUES (?,?,?)", [(1, 20, "b"), (1, 10, "a"), (2, 5, "c")])
    conn.executemany("INSERT INTO grind_measurements VALUES (?,?,?)", [(1, 2, 1.5), (1, 1, 1.0), (2, 1, 9.0)])
    conn.commit()
    conn.close()
    return path


def test_hostile_session_id_fails_int():
    with pytest.raises(ValueError):
        int("1; DROP TABLE x")


def test_hostile_session_id_rejected_and_tables_intact(db):
    load = _load_details_func(db)
    with pytest.raises(ValueError):
        load("1; DROP TABLE grind_events")
    conn = sqlite3.connect(db)
    assert conn.execute("SELECT count(*) FROM grind_events").fetchone()[0] == 3
    conn.close()


def test_bound_query_returns_correct_rows_sorted(db):
    load = _load_details_func(db)
    events, meas = load(1)
    assert [r[1] for r in events.rows] == [10, 20]
    assert [r[1] for r in meas.rows] == [1, 2]
    events2, meas2 = load("2")
    assert len(events2.rows) == 1 and len(meas2.rows) == 1


def test_unknown_session_returns_empty(db):
    events, meas = _load_details_func(db)(99)
    assert events.rows == [] and meas.rows == []


def test_source_uses_bound_param_not_fstring():
    src = (TOOLS_DIR / "streamlit-reports" / "grind_report.py").read_text(encoding="utf-8")
    assert "session_id = {" not in src
    assert "session_id = ?" in src
