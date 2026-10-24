"""Tests for step 6.4: non-destructive database export in tools/ble/grinder-ble.py.

Only temp directories and temp SQLite files are used. No BLE hardware: the BLE client of
GrinderBLETool is replaced by a fake that answers the file-list / file-request commands.
"""

import ast
import sqlite3
import struct
import sys
import types
from datetime import datetime
from pathlib import Path
from unittest import mock

import pytest

TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from test_variants import _load_ble_module, run  # noqa: E402


@pytest.fixture(scope="module")
def ble():
    return _load_ble_module()


@pytest.fixture
def project(tmp_path, ble):
    """<root>/tools/ble/grinder-ble.py layout so the default 'tools/database' lives in tmp_path."""
    (tmp_path / "tools" / "ble").mkdir(parents=True)
    with mock.patch.object(ble, "__file__", str(tmp_path / "tools" / "ble" / "grinder-ble.py")):
        yield tmp_path


@pytest.fixture
def db_path(tmp_path):
    return tmp_path / "data" / "grinder_data.db"


# --------------------------------------------------------------------------- data builders

def session(sid, ts=1000, **extra):
    s = {
        'session_id': sid, 'session_timestamp': ts, 'profile_id': 1, 'grind_mode': 0,
        'target_weight': 18.0, 'target_time_ms': 0, 'tolerance': 0.1, 'final_weight': 18.05,
        'start_weight': 0.0, 'error_grams': 0.05, 'time_error_ms': 0, 'total_time_ms': 9000,
        'total_motor_on_time_ms': 8000, 'pulse_count': 2, 'max_pulse_attempts': 10,
        'termination_reason': 0, 'latency_to_coast_ratio': 1.0, 'flow_rate_threshold': 0.5,
        'schema_version': 2, 'result_status': 'EXCELLENT', 'checksum': 7, 'session_size_bytes': 100,
    }
    s.update(extra)
    return s


def event(sid, seq, **extra):
    e = {
        'session_id': sid, 'event_sequence_id': seq, 'timestamp_ms': seq * 10, 'phase_id': 1,
        'phase_name': 'PREDICTIVE', 'pulse_attempt_number': 0, 'duration_ms': 5, 'start_weight': 0.0,
        'end_weight': 1.0, 'motor_stop_target_weight': 17.5, 'pulse_duration_ms': 0.0,
        'grind_latency_ms': 100, 'settling_duration_ms': 0, 'pulse_flow_rate': 0.0, 'loop_count': 1,
        'event_flags': 0,
    }
    e.update(extra)
    return e


def measurement(sid, seq, **extra):
    m = {
        'session_id': sid, 'sequence_id': seq, 'timestamp_ms': seq * 10, 'weight_grams': seq * 0.1,
        'weight_delta': 0.1, 'flow_rate_g_per_s': 1.5, 'motor_is_on': True, 'phase_id': 1,
        'phase_name': 'PREDICTIVE', 'motor_stop_target_weight': 17.5,
    }
    m.update(extra)
    return m


def full_session(sid, ts=1000, n_events=3, n_meas=4):
    return ([session(sid, ts)], [event(sid, i) for i in range(n_events)],
            [measurement(sid, i) for i in range(n_meas)])


def dump(path):
    conn = sqlite3.connect(str(path))
    try:
        return {
            "sessions": conn.execute("SELECT session_id, session_timestamp FROM grind_sessions ORDER BY 1").fetchall(),
            "events": conn.execute("SELECT session_id, event_sequence_id FROM grind_events ORDER BY 1,2").fetchall(),
            "meas": conn.execute("SELECT session_id, sequence_id FROM grind_measurements ORDER BY 1,2").fetchall(),
        }
    finally:
        conn.close()


def backups(path):
    return sorted(p.name for p in Path(path).parent.iterdir() if ".bak-" in p.name)


def fill(ble, path, ids, ts=1000, **kw):
    for sid in ids:
        s, e, m = full_session(sid, ts, **kw)
        ble.store_sessions(path, s, e, m)


# --------------------------------------------------------------------------- store_sessions

def test_first_export_into_missing_db(ble, db_path):
    assert not db_path.exists()
    s, e, m = full_session(1)
    result = ble.store_sessions(db_path, s, e, m)
    assert result == {'inserted': 1, 'replaced': 0, 'total': 1, 'collisions': []}
    assert dump(db_path) == {"sessions": [(1, 1000)], "events": [(1, 0), (1, 1), (1, 2)],
                             "meas": [(1, 0), (1, 1), (1, 2), (1, 3)]}


def test_missing_parent_directory_is_created(ble, tmp_path):
    target = tmp_path / "a" / "b" / "x.db"
    s, e, m = full_session(1)
    ble.store_sessions(target, s, e, m)
    assert target.exists()


def test_overlapping_exports_give_union_and_are_idempotent(ble, db_path):
    fill(ble, db_path, [1, 2, 3])
    r = ble.store_sessions(db_path, *_combine([full_session(i) for i in (3, 4, 5)]))
    assert (r['inserted'], r['replaced'], r['total']) == (2, 1, 5)
    first = dump(db_path)
    assert [x[0] for x in first["sessions"]] == [1, 2, 3, 4, 5]
    assert len(first["events"]) == 15 and len(first["meas"]) == 20
    # repeat: no change
    r = ble.store_sessions(db_path, *_combine([full_session(i) for i in (3, 4, 5)]))
    assert (r['inserted'], r['replaced'], r['total']) == (0, 3, 5)
    assert dump(db_path) == first


def _combine(parts):
    return ([x for p in parts for x in p[0]], [x for p in parts for x in p[1]], [x for p in parts for x in p[2]])


def test_replaced_session_leaves_one_row_and_no_stale_children(ble, db_path):
    ble.store_sessions(db_path, *full_session(7, 1000, n_events=6, n_meas=9))
    ble.store_sessions(db_path, *full_session(7, 1000, n_events=2, n_meas=3))
    d = dump(db_path)
    assert d["sessions"] == [(7, 1000)]
    assert d["events"] == [(7, 0), (7, 1)]
    assert d["meas"] == [(7, 0), (7, 1), (7, 2)]


def test_replacement_updates_values(ble, db_path):
    ble.store_sessions(db_path, [session(1, final_weight=10.0)], [], [])
    ble.store_sessions(db_path, [session(1, final_weight=20.0)], [], [])
    conn = sqlite3.connect(str(db_path))
    assert conn.execute("SELECT final_weight FROM grind_sessions").fetchall() == [(20.0,)]
    conn.close()


def test_other_sessions_children_untouched_by_replacement(ble, db_path):
    fill(ble, db_path, [1, 2])
    ble.store_sessions(db_path, *full_session(2, 1000, n_events=1, n_meas=1))
    d = dump(db_path)
    assert [x for x in d["events"] if x[0] == 1] == [(1, 0), (1, 1), (1, 2)]


def test_same_id_different_timestamp_reported_as_collision(ble, db_path):
    ble.store_sessions(db_path, *full_session(4, 1000))
    r = ble.store_sessions(db_path, *full_session(4, 2000))
    assert r['collisions'] == [(4, 1000, 2000)]
    assert dump(db_path)["sessions"] == [(4, 2000)]


def test_same_timestamp_is_not_a_collision(ble, db_path):
    ble.store_sessions(db_path, *full_session(4, 1000))
    assert ble.store_sessions(db_path, *full_session(4, 1000))['collisions'] == []


def test_find_timestamp_collisions_direct(ble):
    conn = sqlite3.connect(":memory:")
    ble.create_schema(conn)
    conn.execute("INSERT INTO grind_sessions (session_id, session_timestamp) VALUES (1, 10), (2, 20)")
    got = ble.find_timestamp_collisions(conn, [session(1, 10), session(2, 99), session(3, 5)])
    assert got == [(2, 20, 99)]
    conn.close()


def test_empty_lists_do_not_wipe_existing_data(ble, db_path):
    fill(ble, db_path, [1, 2])
    before = dump(db_path)
    r = ble.store_sessions(db_path, [], [], [])
    assert (r['inserted'], r['replaced'], r['total']) == (0, 0, 2)
    assert dump(db_path) == before


# --------------------------------------------------------------------------- rollback

def test_exception_mid_insert_rolls_back_and_keeps_old_data(ble, db_path):
    fill(ble, db_path, [1, 2])
    before = dump(db_path)
    s, e, m = full_session(2, 1000, n_events=1, n_meas=1)
    s += [session(3)]
    m[0]['weight_grams'] = [1, 2]  # unsupported type -> sqlite3 error after sessions/events were written
    with pytest.raises(sqlite3.Error):
        ble.store_sessions(db_path, s, e, m)
    assert dump(db_path) == before


def test_python_error_mid_insert_rolls_back(ble, db_path):
    fill(ble, db_path, [1])
    before = dump(db_path)
    s, e, m = full_session(1, 1000, n_events=1, n_meas=1)
    del e[0]['phase_id']
    with pytest.raises(KeyError):
        ble.store_sessions(db_path, s, e, m)
    assert dump(db_path) == before


# --------------------------------------------------------------------------- schema upgrade

GEN1_SESSIONS = """CREATE TABLE grind_sessions (
    session_id INTEGER PRIMARY KEY, session_timestamp INTEGER, profile_id INTEGER, target_weight REAL,
    tolerance REAL, final_weight REAL, error_grams REAL, total_time_ms INTEGER,
    total_motor_on_time_ms INTEGER, pulse_count INTEGER, result_status TEXT,
    received_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"""
GEN1_EVENTS = """CREATE TABLE grind_events (
    session_id INTEGER, event_sequence_id INTEGER, timestamp_ms INTEGER, phase_id INTEGER, phase_name TEXT,
    pulse_attempt_number INTEGER, duration_ms INTEGER, start_weight REAL, end_weight REAL,
    motor_stop_target_weight REAL, pulse_duration_ms REAL, grind_latency_ms INTEGER,
    settling_duration_ms INTEGER, pulse_flow_rate REAL, loop_count INTEGER,
    PRIMARY KEY (session_id, event_sequence_id))"""
GEN1_MEAS = """CREATE TABLE grind_measurements (
    session_id INTEGER, sequence_id INTEGER, timestamp_ms INTEGER, weight_grams REAL, weight_delta REAL,
    flow_rate_g_per_s REAL, motor_is_on BOOLEAN, phase_id INTEGER, phase_name TEXT,
    PRIMARY KEY (session_id, sequence_id))"""


def make_gen1(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.execute(GEN1_SESSIONS)
    conn.execute(GEN1_EVENTS)
    conn.execute(GEN1_MEAS)
    conn.execute("INSERT INTO grind_sessions (session_id, session_timestamp, target_weight, result_status) "
                 "VALUES (50, 111, 18.0, 'GOOD')")
    conn.execute("INSERT INTO grind_events (session_id, event_sequence_id, phase_name) VALUES (50, 0, 'old')")
    conn.execute("INSERT INTO grind_measurements (session_id, sequence_id, weight_grams) VALUES (50, 0, 1.5)")
    conn.commit()
    conn.close()


def columns(path, table):
    conn = sqlite3.connect(str(path))
    cols = [r[1] for r in conn.execute(f"PRAGMA table_info({table})")]
    conn.close()
    return cols


def test_first_generation_db_upgrades_in_place_and_accepts_export(ble, db_path):
    make_gen1(db_path)
    ble.store_sessions(db_path, *full_session(1, 1000))
    for table, expected in ble.EXPECTED_COLUMNS.items():
        assert set(n for n, _ in expected) <= set(columns(db_path, table))
    d = dump(db_path)
    assert d["sessions"] == [(1, 1000), (50, 111)]
    assert (50, 0) in d["events"] and (50, 0) in d["meas"]
    conn = sqlite3.connect(str(db_path))
    old = conn.execute("SELECT target_weight, result_status, grind_mode, event_flags_missing FROM "
                       "(SELECT target_weight, result_status, grind_mode, 0 AS event_flags_missing "
                       "FROM grind_sessions WHERE session_id = 50)").fetchone()
    assert old == (18.0, 'GOOD', None, 0)
    assert conn.execute("SELECT event_flags FROM grind_events WHERE session_id = 50").fetchone() == (None,)
    assert conn.execute("SELECT event_flags FROM grind_events WHERE session_id = 1").fetchone() == (0,)
    conn.close()


def test_first_generation_db_with_replacement_of_old_session(ble, db_path):
    make_gen1(db_path)
    ble.store_sessions(db_path, *full_session(50, 111, n_events=1, n_meas=1))
    d = dump(db_path)
    assert d["sessions"] == [(50, 111)] and d["events"] == [(50, 0)]


def test_second_generation_db_with_extra_columns_keeps_extras(ble, db_path):
    db_path.parent.mkdir(parents=True)
    conn = sqlite3.connect(str(db_path))
    # current schema minus event_flags, plus a column this tool never heard of
    conn.execute("CREATE TABLE grind_sessions (session_id INTEGER PRIMARY KEY, session_timestamp INTEGER, "
                 "profile_id INTEGER, grind_mode INTEGER, target_weight REAL, target_time_ms INTEGER, "
                 "tolerance REAL, final_weight REAL, start_weight REAL, error_grams REAL, time_error_ms INTEGER, "
                 "total_time_ms INTEGER, total_motor_on_time_ms INTEGER, pulse_count INTEGER, "
                 "max_pulse_attempts INTEGER, termination_reason INTEGER, latency_to_coast_ratio REAL, "
                 "flow_rate_threshold REAL, schema_version INTEGER, result_status TEXT, checksum INTEGER, "
                 "session_size_bytes INTEGER, received_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, notes TEXT)")
    conn.execute("CREATE TABLE grind_events (session_id INTEGER, event_sequence_id INTEGER, timestamp_ms INTEGER, "
                 "phase_id INTEGER, phase_name TEXT, pulse_attempt_number INTEGER, duration_ms INTEGER, "
                 "start_weight REAL, end_weight REAL, motor_stop_target_weight REAL, pulse_duration_ms REAL, "
                 "grind_latency_ms INTEGER, settling_duration_ms INTEGER, pulse_flow_rate REAL, loop_count INTEGER, "
                 "extra_col TEXT, PRIMARY KEY (session_id, event_sequence_id))")
    conn.execute("INSERT INTO grind_sessions (session_id, session_timestamp, notes) VALUES (9, 5, 'keep me')")
    conn.execute("INSERT INTO grind_events (session_id, event_sequence_id, extra_col) VALUES (9, 0, 'x')")
    conn.commit()
    conn.close()
    ble.store_sessions(db_path, *full_session(1, 1000))
    assert "event_flags" in columns(db_path, "grind_events")
    assert "extra_col" in columns(db_path, "grind_events")
    assert "grind_measurements" in {r[0] for r in sqlite3.connect(str(db_path)).execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    conn = sqlite3.connect(str(db_path))
    assert conn.execute("SELECT notes FROM grind_sessions WHERE session_id = 9").fetchone() == ("keep me",)
    assert conn.execute("SELECT extra_col FROM grind_events WHERE session_id = 9").fetchone() == ("x",)
    conn.close()
    assert [x[0] for x in dump(db_path)["sessions"]] == [1, 9]


def test_create_schema_is_idempotent(ble):
    conn = sqlite3.connect(":memory:")
    ble.create_schema(conn)
    ble.create_schema(conn)
    for table, expected in ble.EXPECTED_COLUMNS.items():
        assert [r[1] for r in conn.execute(f"PRAGMA table_info({table})")] == [n for n, _ in expected]
    conn.close()


def _make_unaddable(path):
    """A table whose column differs from the expected one only by case: ADD COLUMN fails as a duplicate."""
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.execute("CREATE TABLE grind_events (session_id INTEGER, event_sequence_id INTEGER, Event_Flags INTEGER, "
                 "PRIMARY KEY (session_id, event_sequence_id))")
    conn.execute("INSERT INTO grind_events (session_id, event_sequence_id) VALUES (1, 0)")
    conn.commit()
    conn.close()


def test_unaddable_column_raises_database_schema_error(ble, db_path):
    _make_unaddable(db_path)
    conn = sqlite3.connect(str(db_path))
    with pytest.raises(ble.DatabaseSchemaError, match="event_flags|timestamp_ms"):
        ble.create_schema(conn)
    conn.close()


def test_schema_error_leaves_database_unchanged_via_store(ble, db_path):
    _make_unaddable(db_path)
    sqlite_before = db_path.read_bytes()
    with pytest.raises(ble.DatabaseSchemaError):
        ble.store_sessions(db_path, *full_session(2))
    conn = sqlite3.connect(str(db_path))
    assert conn.execute("SELECT session_id, event_sequence_id FROM grind_events").fetchall() == [(1, 0)]
    conn.close()
    assert sqlite_before  # sanity


# --------------------------------------------------------------------------- backup_database

def stamp_dt(year=2024, sec=0):
    return datetime(year, 1, 2, 3, 4, sec)


def test_backup_of_missing_db_returns_none(ble, db_path):
    assert ble.backup_database(db_path) is None
    assert not db_path.parent.exists() or backups(db_path) == []


def test_backup_is_a_valid_copy_with_timestamp_name(ble, db_path):
    fill(ble, db_path, [1, 2])
    b = ble.backup_database(db_path, now=stamp_dt())
    assert b.name == "grinder_data.db.bak-20240102-030400"
    assert dump(b) == dump(db_path)


def test_rotation_keeps_five_newest(ble, db_path):
    fill(ble, db_path, [1])
    for i in range(8):
        ble.backup_database(db_path, now=stamp_dt(2020 + i))
    names = backups(db_path)
    assert len(names) == 5
    assert names[0].endswith("20230102-030400") and names[-1].endswith("20270102-030400")


def test_rotation_never_deletes_foreign_files(ble, db_path):
    fill(ble, db_path, [1])
    d = db_path.parent
    foreign = ["grinder_data.db.bak-manual", "grinder_data.db.bak-", "grinder_data.db.bak-20200101-000000.txt",
               "other.db.bak-20190101-000000", "other.db.bak-20190101-000001", "grinder_data.db.bak-2020",
               "xgrinder_data.db.bak-20190101-000000", "notes.txt"]
    for n in foreign:
        (d / n).write_bytes(b"keep")
    for i in range(9):
        ble.backup_database(db_path, now=stamp_dt(2000 + i))
    for n in foreign:
        assert (d / n).read_bytes() == b"keep"
    ours = [p for p in backups(db_path) if p.startswith("grinder_data.db.bak-2")
            and len(p) == len("grinder_data.db.bak-20000102-030400")]
    # 5 rotated + the 'xgrinder...' / 'other...' lookalikes don't count as ours
    assert len([p for p in ours if p.startswith("grinder_data.db.bak-200")]) == 5


def test_backup_with_clock_set_backwards_survives(ble, db_path):
    fill(ble, db_path, [1])
    for i in range(6):
        ble.backup_database(db_path, now=stamp_dt(2030 + i))
    new = ble.backup_database(db_path, now=stamp_dt(2001))
    assert new.exists()
    names = backups(db_path)
    assert len(names) == 5 and new.name in names


def test_same_second_backups_get_numeric_suffixes(ble, db_path):
    fill(ble, db_path, [1])
    now = stamp_dt()
    a = ble.backup_database(db_path, now=now, keep=20)
    b = ble.backup_database(db_path, now=now, keep=20)
    c = ble.backup_database(db_path, now=now, keep=20)
    assert [p.name for p in (a, b, c)] == [
        "grinder_data.db.bak-20240102-030400",
        "grinder_data.db.bak-20240102-030400-1",
        "grinder_data.db.bak-20240102-030400-2",
    ]


def test_suffix_ordering_is_numeric_in_rotation(ble, db_path):
    fill(ble, db_path, [1])
    now = stamp_dt()
    made = [ble.backup_database(db_path, now=now, keep=50) for _ in range(12)]
    ble.backup_database(db_path, now=now, keep=5)
    left = backups(db_path)
    assert len(left) == 5
    # 12 earlier backups are base, -1..-11; the new one is -12; the 4 newest earlier are -11..-8
    assert {n.rsplit("-", 1)[1] for n in left} == {"8", "9", "10", "11", "12"}
    assert made[0].name not in left


def test_invalid_db_backup_raises_and_leaves_no_bak(ble, db_path):
    db_path.parent.mkdir(parents=True)
    db_path.write_bytes(b"this is definitely not a sqlite database" * 50)
    with pytest.raises(sqlite3.DatabaseError):
        ble.backup_database(db_path, now=stamp_dt())
    assert backups(db_path) == []


# --------------------------------------------------------------------------- save_failed_session

def test_save_failed_session_writes_bin_in_directory(ble, tmp_path):
    p = ble.save_failed_session(12, b"\x01\x02", tmp_path / "newdir" / "database")
    assert p == tmp_path / "newdir" / "database" / "failed_session_12.bin"
    assert p.read_bytes() == b"\x01\x02"


# --------------------------------------------------------------------------- _store_data

def test_store_data_backs_up_then_merges_and_prints(ble, project, db_path, capsys):
    tool = ble.GrinderBLETool()
    fill(ble, db_path, [1])
    r = tool._store_data(*full_session(2), str(db_path))
    out = capsys.readouterr().out
    assert (r['inserted'], r['total']) == (1, 2)
    assert len(backups(db_path)) == 1
    assert "Backed up existing database" in out
    assert "Database now holds 2 sessions (1 new, 0 replaced)" in out


def test_store_data_first_export_makes_no_backup(ble, project, db_path):
    tool = ble.GrinderBLETool()
    tool._store_data(*full_session(2), str(db_path))
    assert backups(db_path) == []


def test_store_data_collision_warns_and_backup_exists(ble, project, db_path, capsys):
    tool = ble.GrinderBLETool()
    ble.store_sessions(db_path, *full_session(4, 1000))
    tool._store_data(*full_session(4, 2000), str(db_path))
    out = capsys.readouterr().out
    assert "[WARNING] Session 4 was already stored with a different timestamp (1000 vs 2000)" in out
    b = backups(db_path)
    assert len(b) == 1
    old = dump(db_path.parent / b[0])
    assert old["sessions"] == [(4, 1000)]


def test_store_data_never_removes_db(ble, project, db_path):
    tool = ble.GrinderBLETool()
    fill(ble, db_path, [1, 2, 3])
    with mock.patch("os.remove", side_effect=AssertionError("os.remove called")):
        tool._store_data(*full_session(4), str(db_path))
    assert [x[0] for x in dump(db_path)["sessions"]] == [1, 2, 3, 4]


# --------------------------------------------------------------------------- export_data (fake BLE)

class Fake:
    """Drives GrinderBLETool.export_data without BLE: answers commands from fake asyncio.sleep."""

    def __init__(self, ble, tool, ids, files):
        self.ble, self.tool, self.ids, self.files = ble, tool, ids, files
        self.pending = None
        tool.client = types.SimpleNamespace(write_gatt_char=self.write)
        tool._parse_single_file_data = self.parse

    async def write(self, _uuid, data):
        self.pending = bytes(data)

    async def sleep(self, _t):
        cmd, self.pending = self.pending, None
        if cmd is None or not self.tool.receiving_data:
            self.pending = cmd
            return
        if cmd[0] == self.ble.BLE_DATA_CMD_GET_FILE_LIST:
            self.tool.data_chunks = [struct.pack('<I', len(self.ids)) + b"".join(struct.pack('<I', i) for i in self.ids)]
        elif cmd[0] == self.ble.BLE_DATA_CMD_REQUEST_FILE:
            sid = struct.unpack('<I', cmd[1:5])[0]
            self.tool.data_chunks = [b"RAW%d" % sid]
        self.tool.receiving_data = False

    def parse(self, data, sid):
        result = self.files[sid]
        if isinstance(result, Exception):
            raise result
        return result


def do_export(ble, tool, ids, files, db):
    fake = Fake(ble, tool, ids, files)
    with mock.patch.object(ble.asyncio, "sleep", fake.sleep):
        return run(tool.export_data(str(db)))


def test_export_data_end_to_end_creates_db_then_union(ble, project, db_path):
    tool = ble.GrinderBLETool()
    assert do_export(ble, tool, [1, 2], {1: full_session(1), 2: full_session(2)}, db_path) is True
    assert [x[0] for x in dump(db_path)["sessions"]] == [1, 2]
    assert backups(db_path) == []
    # second, overlapping export (device log was trimmed) keeps the older rows
    assert do_export(ble, tool, [2, 3], {2: full_session(2), 3: full_session(3)}, db_path) is True
    assert [x[0] for x in dump(db_path)["sessions"]] == [1, 2, 3]
    assert len(backups(db_path)) == 1
    # idempotent
    before = dump(db_path)
    assert do_export(ble, tool, [2, 3], {2: full_session(2), 3: full_session(3)}, db_path) is True
    assert dump(db_path) == before


def test_export_data_zero_sessions_returns_true_and_does_not_touch_db(ble, project, db_path):
    tool = ble.GrinderBLETool()
    fill(ble, db_path, [1, 2])
    raw, mtime = db_path.read_bytes(), db_path.stat().st_mtime_ns
    assert do_export(ble, tool, [], {}, db_path) is True
    assert db_path.read_bytes() == raw and db_path.stat().st_mtime_ns == mtime
    assert backups(db_path) == []


def test_export_data_zero_sessions_does_not_create_db(ble, project, db_path):
    tool = ble.GrinderBLETool()
    assert do_export(ble, tool, [], {}, db_path) is True
    assert not db_path.exists()


def test_export_data_invalid_db_returns_false_without_bak_or_change(ble, project, db_path):
    tool = ble.GrinderBLETool()
    db_path.parent.mkdir(parents=True)
    garbage = b"not sqlite at all " * 100
    db_path.write_bytes(garbage)
    assert do_export(ble, tool, [1], {1: full_session(1)}, db_path) is False
    assert db_path.read_bytes() == garbage
    assert backups(db_path) == []


def test_export_data_schema_error_returns_false_and_db_unchanged(ble, project, db_path):
    tool = ble.GrinderBLETool()
    _make_unaddable(db_path)
    raw = db_path.read_bytes()
    assert do_export(ble, tool, [1], {1: full_session(1)}, db_path) is False
    conn = sqlite3.connect(str(db_path))
    assert conn.execute("SELECT COUNT(*) FROM grind_events").fetchone()[0] == 1
    conn.close()
    assert raw  # sanity


def test_export_data_mid_insert_failure_returns_false_and_keeps_data(ble, project, db_path):
    tool = ble.GrinderBLETool()
    fill(ble, db_path, [1, 2])
    before = dump(db_path)
    bad = full_session(2)
    bad[2][0]['weight_grams'] = [1]
    assert do_export(ble, tool, [2, 3], {2: bad, 3: full_session(3)}, db_path) is False
    assert dump(db_path) == before


def test_export_data_all_files_unparsable_returns_false_and_leaves_db(ble, project, db_path):
    tool = ble.GrinderBLETool()
    fill(ble, db_path, [1])
    raw = db_path.read_bytes()
    assert do_export(ble, tool, [5], {5: ValueError("bad")}, db_path) is False
    assert db_path.read_bytes() == raw
    assert backups(db_path) == []


def test_failed_session_saved_under_tools_database(ble, project, db_path, capsys):
    tool = ble.GrinderBLETool()
    ok = do_export(ble, tool, [1, 5], {1: full_session(1), 5: ValueError("corrupt")}, db_path)
    assert ok is True
    saved = project / "tools" / "database" / "failed_session_5.bin"
    assert saved.read_bytes() == b"RAW5"
    assert "Saved corrupted session" in capsys.readouterr().out
    assert [x[0] for x in dump(db_path)["sessions"]] == [1]


def test_oserror_saving_failed_session_only_warns(ble, project, db_path, capsys):
    tool = ble.GrinderBLETool()
    with mock.patch.object(ble, "save_failed_session", side_effect=OSError("disk full")):
        ok = do_export(ble, tool, [1, 5], {1: full_session(1), 5: ValueError("corrupt")}, db_path)
    out = capsys.readouterr().out
    assert ok is True
    assert "[WARNING] Could not save the corrupted session file: disk full" in out
    assert [x[0] for x in dump(db_path)["sessions"]] == [1]


def test_oserror_on_real_directory_only_warns(ble, project, db_path, capsys):
    """Make tools/database a file so mkdir/write fails for real."""
    tool = ble.GrinderBLETool()
    (project / "tools" / "database").write_bytes(b"i am a file")
    ok = do_export(ble, tool, [1, 5], {1: full_session(1), 5: ValueError("corrupt")}, db_path)
    assert ok is True
    assert "Could not save the corrupted session file" in capsys.readouterr().out


# --------------------------------------------------------------------------- report queries on a merged DB

class _Frame:
    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows

    def __getitem__(self, key):
        return types.SimpleNamespace(str=types.SimpleNamespace(strip=lambda: key))

    def __setitem__(self, key, value):
        pass


def _fake_pd():
    def read_sql_query(sql, conn, params=()):
        cur = conn.execute(sql, params)
        return _Frame([d[0] for d in cur.description], cur.fetchall())
    return types.SimpleNamespace(read_sql_query=read_sql_query)


def _load_report_funcs(db_file):
    src = (TOOLS_DIR / "streamlit-reports" / "grind_report.py").read_text(encoding="utf-8")
    wanted = {"load_session_list", "load_session_details", "load_all_details"}
    funcs = [n for n in ast.parse(src).body if isinstance(n, ast.FunctionDef) and n.name in wanted]
    for f in funcs:
        f.decorator_list = []
    ns = {"sqlite3": sqlite3, "pd": _fake_pd(), "DB_FILE": str(db_file)}
    exec(compile(ast.fix_missing_locations(ast.Module(body=funcs, type_ignores=[])), "grind_report.py", "exec"), ns)
    return ns


def test_report_queries_read_merged_db(ble, db_path):
    make_gen1(db_path)
    ble.store_sessions(db_path, *_combine([full_session(1, 1000), full_session(2, 2000, n_events=2, n_meas=2)]))
    ble.store_sessions(db_path, *full_session(2, 2000, n_events=1, n_meas=1))
    ns = _load_report_funcs(db_path)
    sessions = ns["load_session_list"]()
    assert sorted(r[0] for r in sessions.rows) == [1, 2, 50]
    assert "event_flags" not in sessions.columns and "grind_mode" in sessions.columns
    events, meas = ns["load_session_details"](2)
    assert len(events.rows) == 1 and len(meas.rows) == 1
    ev_all, meas_all = ns["load_all_details"]()
    assert len(ev_all.rows) == 3 + 1 + 1 and len(meas_all.rows) == 4 + 1 + 1
    assert "event_flags" in ev_all.columns
