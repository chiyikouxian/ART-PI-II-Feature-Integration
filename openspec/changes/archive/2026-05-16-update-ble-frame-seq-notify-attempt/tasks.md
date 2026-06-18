## 1. Specification
- [x] 1.1 Update the BLE IMU uplink payload contract to describe `frame_seq` as a notify-attempt counter.
- [x] 1.2 Document that `frame_seq` does not advance when no notify call is attempted.

## 2. Validation
- [x] 2.1 Validate the OpenSpec change with `openspec validate --strict --no-interactive`.
