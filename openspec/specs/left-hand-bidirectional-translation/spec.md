# left-hand-bidirectional-translation Specification

## Purpose
Define the current firmware-side bidirectional translation contracts for the dual ART-Pi2 wearable endpoints, including left-hand BLE IMU uplink behavior, BLE text downlink behavior, left-hand translated-text TCP upload semantics, and reconnect handling.
## Requirements
### Requirement: BLE Module Availability And Startup Activation
The current firmware SHALL keep the BLE IMU service implementation available in the codebase while allowing the default main startup path to leave BLE initialization disabled.

#### Scenario: Default startup path does not automatically initialize BLE
- **WHEN** the current default left-hand or right-hand `main.c` startup path runs without local source modification
- **THEN** the main flow does not automatically call `left_ble_app_init()` or `right_ble_app_init()`
- **AND** WiFi/TCP startup continues as the active runtime path
- **AND** the BLE module code remains present for later enablement

#### Scenario: BLE behavior applies after explicit BLE initialization
- **WHEN** a firmware build or source edit explicitly invokes the corresponding BLE app initialization function
- **THEN** the BLE advertising, GATT service, text downlink, and IMU notify behavior defined by this specification applies

### Requirement: Left-Hand BLE IMU Uplink Payload Contract
The system SHALL expose the left-hand BLE IMU uplink payload as a fixed-order UTF-8 CSV text frame delivered through the existing NimBLE GATT Notify path, using Service UUID `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00` and IMU Notify Characteristic UUID `A74D0002-B4E7-4C5F-9D2A-F163E80ACB00`. The left-hand firmware SHALL use connectable undirected legacy advertising with device name `ART-Pi2-IMU-L`, and SHALL identify itself from the right-hand firmware by BLE device name, controller-provided BLE address, and the payload `hand_type` value while sharing the same GATT UUID set used by the right-hand firmware. The advertising address type SHALL be inferred by NimBLE from the CYW43438 controller, the effective address SHALL be read from the controller and logged after host synchronization, and no fixed BLE address SHALL be guaranteed by the firmware contract. The notify cadence SHALL be 90 ms per frame. The payload SHALL carry raw unfiltered IMU sample values and include a per-hand frame sequence number. Code-level UUID byte arrays are written with LSB-first ordering; the canonical UUID display form is `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`.

#### Scenario: A normal left-hand IMU notify frame preserves fixed field order and count with frame sequence
- **WHEN** the left-hand endpoint emits one IMU uplink payload through the BLE notify characteristic
- **THEN** the payload begins with the literal prefix `[DATA]`
- **AND** the remaining payload is emitted as UTF-8 CSV text in the fixed field order `<timestamp_ms>,<hand_type>,<frame_seq>,ch0_ax,ch0_ay,ch0_az,ch0_gx,ch0_gy,ch0_gz,...,ch9_ax,ch9_ay,ch9_az,ch9_gx,ch9_gy,ch9_gz,ch10_ax,ch10_ay,ch10_az,ch10_gx,ch10_gy,ch10_gz,ch10_mx,ch10_my,ch10_mz`
- **AND** the payload ends with `\n`
- **AND** `timestamp_ms` is the decimal ASCII rendering of the `rt_tick` millisecond value as a monotonically increasing `uint32`
- **AND** `hand_type` is the literal `left`
- **AND** `frame_seq` is a decimal ASCII rendering of a per-hand `uint32` frame counter that advances once after at least one notify call has been attempted for that frame and wraps to 0 on overflow
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
- **AND** the left-hand firmware infers its advertising address type from the CYW43438 controller and logs the controller-provided BLE address after NimBLE host synchronization
- **AND** the left-hand payload identifies the source hand by emitting `hand_type` as `left`
- **AND** the right-hand firmware is distinguished from the left-hand firmware by BLE device name, controller-provided BLE address, and `hand_type` value rather than by different GATT UUIDs
- **AND** neither endpoint relies on a hardcoded BLE address in this contract

#### Scenario: BLE uses the implemented legacy advertising path
- **WHEN** explicit BLE initialization completes and the NimBLE host synchronizes with the CYW43438 controller
- **THEN** the endpoint SHALL start connectable undirected general-discoverable advertising through `ble_gap_adv_start`
- **AND** the configured advertising interval SHALL range from 200 ms to 500 ms
- **AND** this contract SHALL NOT claim use of extended advertising or another BLE 5-specific feature that the current implementation does not configure

#### Scenario: The IMU uplink uses notify-only frame delivery at a fixed cadence
- **WHEN** the left-hand endpoint delivers IMU uplink payloads over BLE
- **THEN** each payload frame is delivered by BLE GATT Notify rather than indication
- **AND** the endpoint emits frames at a 90 ms interval, approximately 11.1 Hz
- **AND** the endpoint does not request an ACK or retransmit a frame at the notify payload layer
- **AND** the ROCK-side consumer does not need to write an acknowledgment back to the IMU notify characteristic

#### Scenario: Oversized BLE CSV frames are fragmented before notify
- **WHEN** one complete BLE IMU CSV frame exceeds the safe Notify payload size
- **THEN** the endpoint splits the frame into multiple notifications using the text format `[FRAG]<frame_seq>,<hand_type>,<frag_idx>,<frag_total>,<payload_part>\n`
- **AND** all fragments belonging to one complete frame carry the same `frame_seq`
- **AND** `frag_idx` is 0-based
- **AND** `frag_total` is the total fragment count for that frame
- **AND** each fragment length is less than or equal to 180 bytes

#### Scenario: The frame sequence advances after attempted fragment notify calls
- **WHEN** the left-hand endpoint has built one BLE IMU CSV frame and starts sending one or more fragments for that frame
- **THEN** the endpoint advances `frame_seq` exactly once after the fragment send loop finishes if at least one fragment reached the notify API call
- **AND** the endpoint advances `frame_seq` even when one or more fragment notify calls report failure
- **AND** the endpoint does not advance `frame_seq` when no fragment reaches the notify API because the BLE link is not connected, the notify characteristic is not subscribed, or every fragment mbuf allocation fails
- **AND** if `BLE_HS_ENOTCONN` is reported for one fragment notify call, the endpoint stops sending the remaining fragments for that frame

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
- **AND** the PC-facing JSON TCP uplink continues to use the processed values returned by `mpu_get_channel_data`, including the configured filtering and gyroscope bias correction
- **AND** the ROCK-facing WiFi/TCP CSV uplink and the BLE Notify CSV uplink both use raw values returned by `mpu_get_channel_raw_data`

### Requirement: Left-Hand BLE Disconnect Recovery
The left-hand endpoint SHALL automatically resume advertising after a BLE disconnection so that ROCK can reconnect without requiring a device restart.

#### Scenario: BLE link drops and advertising restarts
- **WHEN** the BLE link between the left-hand endpoint and ROCK is disconnected
- **THEN** the endpoint receives a `BLE_GAP_EVENT_DISCONNECT` event
- **AND** the endpoint clears the active connection handle and subscription state
- **AND** the endpoint immediately restarts BLE advertising so that ROCK can reconnect
- **AND** the endpoint does not require a firmware restart or manual intervention to resume operation

#### Scenario: IMU notify resumes after reconnection
- **WHEN** ROCK reconnects and re-subscribes to the IMU notify characteristic after a prior disconnect
- **THEN** the endpoint resumes sending IMU notify frames as normal
- **AND** the frame_seq counter continues from where it left off before the disconnect

### Requirement: Missing Translated Text Handling
The left-hand endpoint SHALL omit the `translated_text` field from the TCP JSON payload when no translated text has been received from ROCK, preserving backward compatibility with PC-side consumers that do not expect the field on every frame.

#### Scenario: translated_text field is absent when no text has been received
- **WHEN** the left-hand endpoint builds a TCP JSON payload and `translated_text_pending` is false
- **THEN** the `translated_text` field SHALL NOT appear in the JSON payload
- **AND** the payload SHALL still contain the existing left-hand sensor object and battery data
- **AND** PC-side consumers that do not handle the `translated_text` field continue to operate without error

#### Scenario: translated_text field appears only when new text is available
- **WHEN** the left-hand endpoint has received translated text from ROCK and `translated_text_pending` is true
- **THEN** the `translated_text` field SHALL appear as an additive field in the next TCP JSON payload
- **AND** the field SHALL be cleared after it has been successfully sent once

### Requirement: TCP Unavailability Handling
The left-hand endpoint SHALL automatically retry the TCP connection to the PC when the connection is unavailable, without requiring a firmware restart.

#### Scenario: TCP connection failure triggers automatic retry
- **WHEN** the TCP connection to the PC server cannot be established or is lost during operation
- **THEN** the endpoint closes the failed socket
- **AND** the endpoint waits a fixed interval before retrying the connection
- **AND** the endpoint continues retrying until the connection is restored
- **AND** no firmware restart or manual intervention is required to resume TCP uploads

#### Scenario: Buffered translated text survives a TCP reconnect
- **WHEN** a TCP connection loss occurs while translated text is pending upload
- **THEN** the pending `translated_text` is retained in the endpoint's internal buffer
- **AND** the text is included in the next TCP payload after the connection is restored
