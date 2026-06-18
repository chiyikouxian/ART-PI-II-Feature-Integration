## ADDED Requirements

### Requirement: Left-Hand IMU Acquisition
The system SHALL allow the left-hand endpoint to continuously collect left-hand IMU data using the existing left-hand sensor acquisition path with real-time digital filtering.

#### Scenario: Left-hand IMU collection is active
- **WHEN** the left-hand endpoint is running normally
- **THEN** it collects left-hand IMU data for the configured left-hand sensor set
- **AND** the collected data remains available to the existing BLE uplink and TCP uplink paths
- **AND** the data is processed through a first-order IIR low-pass filter before transmission

#### Scenario: IMU data filtering is applied
- **WHEN** IMU data is queried via `mpu_get_channel_data()`
- **THEN** the system applies exponential moving average filtering with alpha=0.25 for MPU6050 sensors
- **AND** applies stronger filtering with alpha=0.10 for ICM-20948 sensor to reduce jitter
- **AND** automatically subtracts gyroscope bias after calibration (125 samples for MPU6050, 250 samples for ICM-20948)
- **AND** preserves raw unfiltered data for offline analysis via `data_start` command

### Requirement: Left-Hand BLE IMU Uplink To ROCK
The system SHALL allow the left-hand endpoint to send collected IMU data to the ROCK edge device through the existing BLE communication framework.

#### Scenario: IMU data is forwarded to ROCK
- **WHEN** the left-hand endpoint has collected IMU data and the BLE link to ROCK is available
- **THEN** the left-hand endpoint forwards the IMU data to ROCK through the existing BLE path
- **AND** the change does not introduce a replacement transport or a parallel communication framework

### Requirement: Left-Hand TCP Uplink To PC
The system SHALL allow the left-hand endpoint to send IMU data and battery data to the PC through the existing TCP communication framework.

#### Scenario: IMU and battery data are uploaded to the PC
- **WHEN** the left-hand endpoint has current IMU data and battery data and the TCP connection to the PC is available
- **THEN** it uploads IMU data and battery data through the existing TCP client path
- **AND** the upload remains compatible with the current PC-side TCP ingestion model

### Requirement: Left-Hand BLE Text Downlink From ROCK
The system SHALL allow the left-hand endpoint to receive processed text returned from the ROCK edge device through the existing BLE communication framework.

#### Scenario: Processed text is received from ROCK
- **WHEN** ROCK returns processed text over BLE to the left-hand endpoint
- **THEN** the left-hand endpoint receives that text through the existing BLE path
- **AND** the received text becomes available to both audio playback and TCP upload flows

### Requirement: Left-Hand VTX316 Playback
The system SHALL play processed text received from ROCK through the existing VTX316 speaker path on the left-hand endpoint.

#### Scenario: Received text is spoken locally
- **WHEN** the left-hand endpoint receives processed text from ROCK
- **THEN** it sends that text to the VTX316 playback path
- **AND** playback uses the existing speaker integration rather than a new audio framework

### Requirement: Left-Hand TCP Upload Of Returned Text
The system SHALL upload BLE-received processed text from the left-hand endpoint to the PC by extending the existing left-hand TCP JSON payload through the existing TCP communication framework.

#### Scenario: Received text is uploaded to the PC
- **WHEN** the left-hand endpoint has received processed text from ROCK and the TCP connection to the PC is available
- **THEN** it uploads that text to the PC through the existing TCP client path as an additive field in the existing left-hand JSON payload
- **AND** the message continues to carry the existing left-hand sensor data and battery data needed by current PC-side consumers
- **AND** the text upload does not replace the existing left-hand payload with a separate text-only message shape

#### Scenario: Current PC latest-data consumers remain compatible
- **WHEN** the PC-side TCP server stores the most recent `device:"left"` message
- **THEN** that record still contains the existing left-hand sensor object expected by current PC-side monitoring and gesture pages
- **AND** translated-text data appears only as an additive extension to that record

### Requirement: Left-Hand Communication Scope Constraint
This change SHALL complete the left-hand runtime flow without changing the existing communication framework.

#### Scenario: Proposal remains within communication boundaries
- **WHEN** implementation work is planned from this proposal
- **THEN** the work reuses the existing BLE framework for ROCK communication
- **AND** reuses the existing TCP framework for PC communication
- **AND** does not introduce a new transport, protocol family, or architectural replacement for the current communication paths

## ADDED Requirements

### Requirement: Left-Hand BLE IMU Uplink Payload Contract
The system SHALL expose the left-hand BLE IMU uplink payload as a fixed-order UTF-8 CSV text frame delivered through the existing BLE GATT Notify path, using Service UUID `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00` and IMU Notify Characteristic UUID `A74D0002-B4E7-4C5F-9D2A-F163E80ACB00`. The left-hand firmware SHALL advertise as `ART-Pi2-IMU-L`, use BLE 5.0, and identify itself from the right-hand firmware by BLE device name, MAC address, and the payload `hand_type` value while sharing the same GATT UUID set used by the right-hand firmware. The left-hand MAC address SHALL be `C0:4E:51:05:34:33`, and the notify cadence SHALL be 90 ms per frame. The payload SHALL carry raw unfiltered IMU sample values and include a per-hand frame sequence number. Code-level UUID byte arrays are written with LSB-first ordering; the canonical UUID display form is `A74D0001-B4E7-4C5F-9D2A-F163E80ACB00`.

#### Scenario: A normal left-hand IMU notify frame preserves fixed field order and count with frame sequence
- **WHEN** the left-hand endpoint emits one IMU uplink payload through the BLE notify characteristic
- **THEN** the payload begins with the literal prefix `[DATA]`
- **AND** the remaining payload is emitted as UTF-8 CSV text in the fixed field order `<timestamp_ms>,<hand_type>,<frame_seq>,ch0_ax,ch0_ay,ch0_az,ch0_gx,ch0_gy,ch0_gz,...,ch9_ax,ch9_ay,ch9_az,ch9_gx,ch9_gy,ch9_gz,ch10_ax,ch10_ay,ch10_az,ch10_gx,ch10_gy,ch10_gz,ch10_mx,ch10_my,ch10_mz`
- **AND** the payload ends with `\n`
- **AND** `timestamp_ms` is the decimal ASCII rendering of the `rt_tick` millisecond value as a monotonically increasing `uint32`
- **AND** `hand_type` is the literal `left`
- **AND** `frame_seq` is a decimal ASCII rendering of a per-hand `uint32` frame counter that increments after each successful notify and wraps to 0 on overflow
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
