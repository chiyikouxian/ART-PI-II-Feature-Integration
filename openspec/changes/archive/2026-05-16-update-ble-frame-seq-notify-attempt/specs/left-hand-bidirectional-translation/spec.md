## MODIFIED Requirements

### Requirement: Left-Hand BLE IMU Uplink Payload Contract
The system SHALL expose the left-hand BLE IMU uplink payload as a fixed-order UTF-8 CSV text frame delivered through the existing BLE GATT Notify path, using Service UUID `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00` and IMU Notify Characteristic UUID `A74D0002-B4E7-4C5F-9D2A-F163E80ACB00`. The left-hand firmware SHALL advertise as `ART-Pi2-IMU-L`, use BLE 5.0, and identify itself from the right-hand firmware by BLE device name, MAC address, and the payload `hand_type` value while sharing the same GATT UUID set used by the right-hand firmware. The left-hand MAC address SHALL be `C0:4E:51:05:34:33`, and the notify cadence SHALL be 90 ms per frame. The payload SHALL carry raw unfiltered IMU sample values and include a per-hand frame sequence number. Code-level UUID byte arrays are written with LSB-first ordering; the canonical UUID display form is `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`.

#### Scenario: A normal left-hand IMU notify frame preserves fixed field order and count with frame sequence
- **WHEN** the left-hand endpoint emits one IMU uplink payload through the BLE notify characteristic
- **THEN** the payload begins with the literal prefix `[DATA]`
- **AND** the remaining payload is emitted as UTF-8 CSV text in the fixed field order `<timestamp_ms>,<hand_type>,<frame_seq>,ch0_ax,ch0_ay,ch0_az,ch0_gx,ch0_gy,ch0_gz,...,ch9_ax,ch9_ay,ch9_az,ch9_gx,ch9_gy,ch9_gz,ch10_ax,ch10_ay,ch10_az,ch10_gx,ch10_gy,ch10_gz,ch10_mx,ch10_my,ch10_mz`
- **AND** the payload ends with `\n`
- **AND** `timestamp_ms` is the decimal ASCII rendering of the `rt_tick` millisecond value as a monotonically increasing `uint32`
- **AND** `hand_type` is the literal `left`
- **AND** `frame_seq` is a decimal ASCII rendering of a per-hand `uint32` frame counter that increments after each BLE notify call is attempted and wraps to 0 on overflow
- **AND** the frame contains 72 CSV fields after the `[DATA]` prefix: 1 timestamp field, 1 hand-type field, 1 frame-sequence field, 60 signed integer fields for `ch0` through `ch9`, and 9 signed integer fields for `ch10`
- **AND** each field separator is exactly one `,` character with no spaces

#### Scenario: Invalid channels are zero-filled without changing payload shape
- **WHEN** the left-hand endpoint emits a BLE IMU notify frame and any channel has `valid=false`
- **THEN** the endpoint keeps the channel position and field count unchanged in the payload
- **AND** each invalid `ch0` through `ch9` channel is emitted as six `0` fields for `ax,ay,az,gx,gy,gz`
- **AND** an invalid `ch10` channel is emitted as nine `0` fields for `ax,ay,az,gx,gy,gz,mx,my,mz`
- **AND** the endpoint does not omit, compress, or reorder fields for missing sensors

#### Scenario: Left-hand and right-hand firmware share the same GATT payload contract and are distinguished by identity fields
- **WHEN** the left-hand firmware and right-hand firmware expose their BLE IMU uplink services
- **THEN** both firmware images use the same GATT UUID set for Service, Notify, Channel, and Text characteristics
- **AND** the left-hand firmware exposes BLE device name `ART-Pi2-IMU-L`
- **AND** the left-hand firmware exposes MAC address `C0:4E:51:05:34:33`
- **AND** the left-hand payload identifies the source hand by emitting `hand_type` as `left`
- **AND** the right-hand firmware is distinguished from the left-hand firmware by BLE device name, MAC address, and `hand_type` value rather than by different GATT UUIDs

#### Scenario: The IMU uplink uses notify-only frame delivery at a fixed cadence without fragmentation or acknowledgment
- **WHEN** the left-hand endpoint delivers IMU uplink payloads over BLE
- **THEN** each payload frame is delivered by BLE GATT Notify rather than indication
- **AND** one complete CSV frame is delivered in one notify operation
- **AND** the endpoint emits frames at a 90 ms interval, approximately 11.1 Hz
- **AND** the endpoint does not fragment a frame, request an ACK, or retransmit a frame at the notify payload layer
- **AND** the ROCK-side consumer does not need to write an acknowledgment back to the IMU notify characteristic

#### Scenario: The frame sequence advances after attempted notify calls
- **WHEN** the left-hand endpoint has built a BLE IMU payload and calls the BLE notify API for that frame
- **THEN** the endpoint advances `frame_seq` once after the notify call returns
- **AND** the endpoint advances `frame_seq` regardless of whether the notify call reports success or failure
- **AND** the endpoint does not advance `frame_seq` when no notify call is attempted because the BLE link is not connected, the notify characteristic is not subscribed, or the notify mbuf cannot be allocated

#### Scenario: The BLE text downlink remains separate from the IMU notify payload
- **WHEN** the ROCK-side peer returns translated text to the left-hand endpoint over BLE
- **THEN** the translated text is delivered by Write to the Text Characteristic in the shared GATT UUID set
- **AND** the text downlink uses the Text Characteristic identified by UUID `A74D0004-B4E7-4C5F-9D2A-F163E80ACB00`
- **AND** the text downlink payload is independent from the IMU notify payload defined by this requirement
- **AND** the IMU notify payload does not carry translated text fields

#### Scenario: Numeric sample values are raw unfiltered integers without unit conversion
- **WHEN** the left-hand endpoint serializes IMU sample values into the BLE CSV payload
- **THEN** each sample value is emitted as a signed decimal integer representing the raw unfiltered sensor reading
- **AND** the BLE payload path does not apply low-pass filtering or gyroscope bias correction to the transmitted values
- **AND** the raw integer semantics remain aligned with the `data_start` serial output format
- **AND** the distinction from TCP uplink is that TCP continues to transmit filtered and bias-corrected values while BLE transmits raw values
