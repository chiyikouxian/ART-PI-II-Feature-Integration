# Change: Update BLE Frame Sequence Notify Attempt Semantics

## Why
The ART-Pi2 BLE notify implementation now advances `frame_seq` after each attempted BLE notify call, regardless of the notify return code. The OpenSpec contract still describes `frame_seq` as incrementing only after a successful notify, so the specification must be brought back in sync with firmware behavior.

## What Changes
- Modify the left-hand BLE IMU uplink payload contract to define `frame_seq` as a per-hand counter that advances after each BLE notify call is attempted.
- Clarify that no increment occurs before a notify call is attempted, such as when the endpoint is not connected, not subscribed, or cannot allocate the notify mbuf.
- Preserve the existing CSV field order, raw BLE payload semantics, GATT UUIDs, notify cadence, and text downlink separation.

## Impact
- Affected specs: `left-hand-bidirectional-translation`
- Affected code: `art_pi2_left/applications/imu_notify_thread.c`, `art_pi2_right/applications/imu_notify_thread.c`
