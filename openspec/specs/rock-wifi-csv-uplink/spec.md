# rock-wifi-csv-uplink Specification

## Purpose
Define the current WiFi/TCP uplink and control contract used by the ART-Pi2 left-hand and right-hand firmware when connected to the ROCK edge device.

> **Dependency notice (state mismatch, not a syntax error):** This current spec references
> `OP_STATE_RUNNING`, `OP_STATE_AUTO_STANDBY`, `OP_STATE_MANUAL_SLEEP`, and the
> `CMD:*` / `MODE:*` control grammar. The capability that fully defines those operation
> states (`add-dual-hand-operation-modes`) is **still an active change** with
> 5 of 31 tasks remaining (manual-mode full hardware acceptance, broad negative-sample
> collection, ROCK second-stage reviewer, end-to-end dual-hand acceptance, and the
> final strict-validation re-run). Treat the SHALL semantics below as the intended
> deployed contract, not as a verified current behaviour, until that change is archived.
> See `openspec/changes/add-dual-hand-operation-modes/tasks.md` for the live task list.

## Requirements
### Requirement: Per-Hand WiFi CSV Uplink To ROCK
Each ART-Pi2 endpoint SHALL expose a dedicated WiFi/TCP uplink to ROCK that sends raw IMU CSV frames at a 90 ms cadence while the local operation mode is in running state.

#### Scenario: Left-hand uplink uses the left ROCK endpoint
- **WHEN** the left-hand firmware starts its IMU WiFi sender
- **THEN** it opens a TCP connection to `192.168.221.239:9101`
- **AND** each uplink frame begins with `[DATA]`
- **AND** the payload field order is `<timestamp_ms>,left,<frame_seq>,<69 raw IMU integers>\n`

#### Scenario: Right-hand uplink uses the right ROCK endpoint
- **WHEN** the right-hand firmware starts its IMU WiFi sender
- **THEN** it opens a TCP connection to `192.168.221.239:9102`
- **AND** each uplink frame begins with `[DATA]`
- **AND** the payload field order is `<timestamp_ms>,right,<frame_seq>,<69 raw IMU integers>\n`

### Requirement: ROCK Uplink Endpoint And Auxiliary TCP Defaults Remain Distinct
The ROCK IMU uplink endpoint and the board-side auxiliary TCP defaults SHALL be treated as separate runtime roles in the current firmware branch.

#### Scenario: ROCK uplink does not use the auxiliary TCP default IP
- **WHEN** the firmware sends raw IMU CSV frames to ROCK through `imu_wifi_sender`
- **THEN** it SHALL use the dedicated ROCK endpoint configuration for the WiFi sender
- **AND** it SHALL NOT derive the ROCK target from the auxiliary TCP default server IP used by other modules

#### Scenario: Auxiliary TCP and STT defaults currently use the board-side helper IP
- **WHEN** the firmware reads its current default auxiliary server configuration from `server_config.h` or `tcp_client.h`
- **THEN** the default board-side helper IP SHALL be `192.168.221.217`
- **AND** the auxiliary TCP monitor default port SHALL be `8266`
- **AND** the auxiliary STT default port SHALL be `8080`
- **AND** these defaults SHALL NOT change the ROCK IMU uplink endpoint at `192.168.221.239:9101/9102`

#### Scenario: Invalid channels are zero-filled without changing frame shape
- **WHEN** either endpoint builds one WiFi CSV uplink frame and any IMU channel is invalid
- **THEN** the endpoint preserves the existing field order and field count
- **AND** invalid `ch0` through `ch9` channels are emitted as six `0` fields each
- **AND** an invalid `ch10` channel is emitted as nine `0` fields

### Requirement: WiFi Uplink State Gating
The WiFi/TCP CSV uplink SHALL be gated by the same operation-mode running state used by the current firmware, even though the BLE-only operation-mode spec was originally narrower.

#### Scenario: Running state enables WiFi CSV transmission
- **WHEN** the local endpoint is in `OP_STATE_RUNNING`
- **THEN** the IMU WiFi sender transmits one CSV frame approximately every 90 ms while the TCP connection is healthy

#### Scenario: Non-running states keep the WiFi link connected but silent
- **WHEN** the local endpoint is in `OP_STATE_AUTO_STANDBY` or `OP_STATE_MANUAL_SLEEP`
- **THEN** the IMU WiFi sender keeps its thread and TCP session available
- **AND** it does not emit IMU CSV frames until the state returns to running

### Requirement: ROCK Downlink Command Handling Over WiFi
Each ART-Pi2 endpoint SHALL accept WiFi/TCP downlink command text from ROCK and apply the same operation control words used by the current firmware state machine.

#### Scenario: WiFi command stream accepts operation-mode control words
- **WHEN** the endpoint receives TCP text containing `CMD:RESET_SEQ`, `CMD:START`, or `CMD:STOP`
- **THEN** it forwards the recognized command to the existing operation-mode command handler
- **AND** the resulting state transitions and frame sequence reset behavior match the local firmware operation-mode implementation

#### Scenario: WiFi command stream accepts remote mode-selection words
- **WHEN** the endpoint receives TCP text containing `MODE:MANUAL` or `MODE:AUTO`
- **THEN** it forwards the recognized command to the existing operation-mode command handler
- **AND** the resulting mode and standby-state transitions match the local firmware operation-mode implementation

#### Scenario: WiFi command stream accepts SAY text on the left hand
- **WHEN** the left-hand endpoint receives TCP text beginning with `SAY:` or `say:`
- **THEN** it strips the prefix and lightweight separators before speaking the remaining text through `vtx316_speak_wait()`
- **AND** it also forwards that translated text into the left-hand TCP JSON upload buffer
- **AND** it notifies the current operation-mode state machine that translated output has been received

#### Scenario: WiFi command stream ignores right-hand speech playback
- **WHEN** the right-hand endpoint receives TCP text beginning with `SAY:` or `say:`
- **THEN** it logs the request for observability
- **AND** it does not perform local speech playback on the right hand
- **AND** it still notifies the current operation-mode state machine that translated output has been received

### Requirement: Auxiliary WiFi Text Uplink
Each ART-Pi2 endpoint SHALL be able to send lightweight non-CSV auxiliary text messages to ROCK over the same WiFi/TCP session for current threshold/model metadata and automatic-flow phase notices.

#### Scenario: Endpoint can queue and send MODEL metadata
- **WHEN** the local firmware prepares runtime threshold/model metadata text for ROCK
- **THEN** it SHALL queue that payload in the WiFi sender
- **AND** the sender SHALL transmit the queued payload on the existing TCP session without changing the CSV IMU payload contract

#### Scenario: Automatic workflow can notify ROCK that collection is waiting to stop
- **WHEN** the local automatic workflow finishes its sign-completion phase and enters waiting-stop
- **THEN** the endpoint SHALL be able to send a `WAITING_STOP:<hand>\n` auxiliary text message to ROCK on the active WiFi/TCP session

### Requirement: WiFi Uplink Reconnect Handling
The IMU WiFi sender SHALL reconnect automatically when the ROCK TCP session is unavailable.

#### Scenario: Connection failure triggers retry loop
- **WHEN** the IMU WiFi sender cannot create or connect its TCP socket, or loses the connection while sending
- **THEN** it closes the failed socket
- **AND** it waits approximately 3 seconds before retrying
- **AND** it continues retrying until the link is restored or the sender is stopped
