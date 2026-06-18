# Change: Define ROCK-side BLE Consumer Contract

## Why

The two ART-Pi2 endpoints already emit a finalized BLE IMU payload contract (see add-left-hand-bidirectional-translation). The ROCK edge device's current BLE receiver is only a skeleton: it reads frames and attempts a naive float() parse. The future ROCK implementation will add per-hand buffers, frame-sequence alignment, timestamp fallback, and linear interpolation for missing frames. This change defines the contract that ROCK MUST satisfy when consuming the BLE payload, so that ROCK-side development has a clear, testable target.

## What Changes

- Add a new capability rock-ble-consumer-contract describing required parsing, alignment, and gap-handling behaviors on the ROCK side.
- Specify how ROCK MUST extract [DATA] prefix, hand_type, frame_seq, and the 69 raw IMU integer fields from each BLE notify frame.
- Specify how ROCK MUST maintain independent left/right buffers, align by frame_seq, fall back to local timestamps when needed, and fill gaps via linear interpolation.
- Explicitly exclude algorithm-level decisions (window size, interpolation neighbors, timestamp tolerance); those are owned by the ROCK implementer.

## Impact

- Affected specs: rock-ble-consumer-contract (new capability)
- Affected code: ROCK-side BLE consumer (currently bluetooth_service/ skeleton on ROCK)
- This change does NOT modify ART-Pi2 firmware. It only formalizes ROCK-side expectations.
