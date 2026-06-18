# Proposal: BLE CSV Fragment Sending and ROCK Fragment Reassembly

## Motivation

The current IMU BLE Notify frame is a single CSV text line:

```
[DATA]<timestamp_ms>,<hand_type>,<frame_seq>,<69 IMU ints>\n
```

This frame is approximately 517 bytes. When the ATT MTU is 247 (the NimBLE default), a single Notify payload is limited to ~244 bytes (MTU - 3 ATT header). Sending a 517-byte value as a single notification exceeds the available space, causing `ble_gattc_notify_custom` to return `BLE_HS_EMSGSIZE` (err 4) and the frame is silently dropped.

## Solution

Fragment large CSV frames into smaller `[FRAG]` text chunks at the ART-Pi2 firmware side, and reassemble them back into the original `[DATA]` CSV at the ROCK side before `parse_frame()`.

This approach:
- Does **not** rely on negotiating a larger MTU
- Uses plain text, consistent with the existing `[DATA]` text-based BLE protocol
- Preserves backward compatibility: direct `[DATA]` frames (if small enough) still pass through unchanged
- Does **not** change BLE UUIDs, CMD commands, or TCP data paths

## Format

```
[FRAG]<frame_seq>,<hand_type>,<frag_idx>,<frag_total>,<payload_part>
```

- `frame_seq` — same uint32 frame sequence across all fragments of one complete frame
- `hand_type` — "left" or "right"
- `frag_idx` — 0-based fragment index
- `frag_total` — total number of fragments for this frame
- `payload_part` — raw bytes slice of the complete `[DATA]...` CSV

Each fragment SHALL be <= 180 bytes total.

## Scope

- ART-Pi2 left/right `imu_notify_thread.c`: fragment CSV before sending
- ROCK `service_main.py`: `FragmentReassembler` before `parse_frame()`
- No changes to BLE UUIDs, TCP, CMD commands, operation mode
