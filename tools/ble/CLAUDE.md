# C++ to Python Struct Alignment Guide

Keep the ESP32 binary log structs in `src/logging/grind_logging.h` aligned with the Python parser `tools/ble/grinder-ble.py`. Mismatches cause parse failures and silently wrong data.

## Rules

- **Every struct change = bump/check `GRIND_LOG_SCHEMA_VERSION` + update the parser + purge old logs.** Old sessions in flash have the old layout and give false results.
- Purge on the device: Menu -> Logs & Data -> **Purge Logs**. Do this after flashing firmware with changed structs and before testing.
- Structs use `#pragma pack(push, 1)` (no padding) and are protected by `static_assert(sizeof(...))` in `grind_logging.h`. When you change a struct, update those asserts and the Python constants together.

## Sizes (must match `static_assert`s and the parser constants)

| Struct | Bytes | Python constant (`grinder-ble.py`) |
|---|---|---|
| `TimeSeriesSessionHeader` | 24 | parsed with `'<IIIIHHHH'` |
| `GrindSession` | 80 | `SESSION_STRUCT_SIZE` |
| `GrindEvent` | 44 | `EVENT_STRUCT_SIZE` |
| `GrindMeasurement` | 24 | `MEASUREMENT_STRUCT_SIZE` |

`LOG_SCHEMA_VERSION = 2` in the parser mirrors `GRIND_LOG_SCHEMA_VERSION`; a mismatch only logs a warning and parsing continues.

## Layout reference (packed offsets)

### TimeSeriesSessionHeader (24)
`session_id` u32 @0, `session_timestamp` u32 @4, `session_size` u32 @8, `checksum` u32 @12, `event_count` u16 @16, `measurement_count` u16 @18, `schema_version` u16 @20, `reserved` u16 @22.

### GrindSession (80)
u32 @0 `session_id`, @4 `session_timestamp`, @8 `target_time_ms`, @12 `total_time_ms`, @16 `total_motor_on_time_ms`; i32 @20 `time_error_ms`;
f32 @24 `target_weight`, @28 `tolerance`, @32 `final_weight`, @36 `error_grams`, @40 `start_weight`, @44 `initial_motor_stop_offset`, @48 `latency_to_coast_ratio`, @52 `flow_rate_threshold`;
u8 @56 `profile_id`, @57 `grind_mode`, @58 `max_pulse_attempts`, @59 `pulse_count`, @60 `termination_reason`, @61-63 `reserved[3]`; char[16] @64-79 `result_status`.

### GrindEvent (44)
u32 @0 `timestamp_ms`, @4 `duration_ms`, @8 `grind_latency_ms`, @12 `settling_duration_ms`; f32 @16 `start_weight`, @20 `end_weight`, @24 `motor_stop_target_weight`, @28 `pulse_duration_ms`, @32 `pulse_flow_rate`; u16 @36 `event_sequence_id`, @38 `loop_count`; u8 @40 `phase_id`, @41 `pulse_attempt_number`, @42 `event_flags`, @43 `reserved`.

### GrindMeasurement (24)
u32 @0 `timestamp_ms`; f32 @4 `weight_grams`, @8 `weight_delta`, @12 `flow_rate_g_per_s`, @16 `motor_stop_target_weight`; u16 @20 `sequence_id`; u8 @22 `motor_is_on`, @23 `phase_id`.

The parser reads fields with explicit `struct.unpack_from` offsets (session ~line 688, events ~756, measurements ~812), so a new field needs an offset added there, not just a size change.

## Workflow

1. Modify structs in `src/logging/grind_logging.h`; update `static_assert`s and schema version.
2. Update `grinder-ble.py` size constants and `unpack_from` offsets.
3. Build and flash (`python3 tools/grinder.py build-upload`).
4. Purge Logs (see above), then run a fresh test grind.
5. Export: `python3 tools/grinder.py export` and check the parsed values.

## Struct layout debug output

`print_struct_layout_debug()` (prints sizes and field offsets) exists only when firmware is built with `-DENABLE_GRIND_DEBUG=1`; otherwise it is an empty inline. It runs at the start of a BLE export. View it with `python3 -m platformio device monitor` (use the project venv's Python).

## Troubleshooting

- `struct.error: unpack_from requires a buffer of X bytes`: a `*_STRUCT_SIZE` constant or the firmware struct size changed; compare with the `static_assert`s.
- Garbage field values or binary in `result_status`: an `unpack_from` offset is wrong; check against the offsets above.
- Works sometimes, fails other times: old-layout sessions remain in flash; purge logs.

## Key files

- `src/logging/grind_logging.h` / `.cpp` - structs, asserts, layout debug
- `tools/ble/grinder-ble.py` - BLE client and parser
- `tools/grinder.py` - unified CLI that calls the BLE tool