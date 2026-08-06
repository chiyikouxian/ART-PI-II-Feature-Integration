# rock-wifi-csv-uplink Specification

## Purpose
Define the current WiFi/TCP uplink and control contract used by the ART-Pi2 left-hand and right-hand firmware when connected to the ROCK edge device.

> **Plan B fixed internal network:** ROCK runs its own WiFi hotspot as the internal
> network for both gloves. The hotspot interface is `wlan1` with a fixed IP
> `192.168.1.1/24` and a field-verified SSID/password (`rockchip_4eabbe` /
> credential stored in firmware, see `main.c` / `net_manager.c`). On power-up
> each glove connects to that hotspot,
> waits for DHCP / WiFi-Ready, then actively opens its ROCK TCP link:
> left → `192.168.1.1:9101`, right → `192.168.1.1:9102`, right STT →
> `http://192.168.1.1:8080/stt`. ROCK's other interfaces (`wlan0` / `wlP2p33s0`)
> connect to external networks; their address changes do NOT affect the internal
> glove ↔ ROCK links. This is a fixed endpoint — the firmware does not derive the
> ROCK address from the DHCP default gateway. The gateway MAY be logged for
> diagnostics (normally `192.168.1.1`). The PC discovery path (UDP `9108` / TCP
> `9109`) remains fully independent of the ROCK endpoint.

> **Dependency notice (state mismatch, not a syntax error):** This current spec references
> `OP_STATE_RUNNING`, `OP_STATE_AUTO_STANDBY`, `OP_STATE_MANUAL_SLEEP`, and the
> `CMD:*` / `MODE:*` control grammar. The capability that fully defines those operation
> states (`add-dual-hand-operation-modes`) is **still an active change** with
> 3 of 31 tasks remaining (broad negative-sample collection, integration of the optional
> ROCK checker as a post-local-wake reviewer, and end-to-end reviewer acceptance).
> Treat the SHALL semantics below as the intended
> deployed contract, not as a verified current behaviour, until that change is archived.
> See `openspec/changes/add-dual-hand-operation-modes/tasks.md` for the live task list.

## Requirements
### Requirement: Per-Hand WiFi CSV Uplink To ROCK
Each ART-Pi2 endpoint SHALL expose a dedicated WiFi/TCP uplink to ROCK that sends raw IMU CSV frames at a 90 ms cadence while the local operation mode is in running state.

#### Scenario: Left-hand uplink uses the left ROCK endpoint
- **WHEN** the left-hand firmware starts its IMU WiFi sender
- **THEN** it opens a TCP connection to `192.168.1.1:9101`
- **AND** each uplink frame begins with `[DATA]`
- **AND** the payload field order is `<timestamp_ms>,left,<frame_seq>,<69 raw IMU integers>\n`

#### Scenario: Right-hand uplink uses the right ROCK endpoint
- **WHEN** the right-hand firmware starts its IMU WiFi sender
- **THEN** it opens a TCP connection to `192.168.1.1:9102`
- **AND** each uplink frame begins with `[DATA]`
- **AND** the payload field order is `<timestamp_ms>,right,<frame_seq>,<69 raw IMU integers>\n`

### Requirement: ROCK Uplink, STT Proxy, And Auxiliary TCP Defaults Remain Distinct
The ROCK IMU uplink endpoint, ROCK-hosted STT proxy, and board-side auxiliary TCP defaults SHALL be treated as explicit runtime roles in the current firmware branch.

#### Scenario: ROCK uplink does not use the auxiliary TCP default IP
- **WHEN** the firmware sends raw IMU CSV frames to ROCK through `imu_wifi_sender`
- **THEN** it SHALL use the dedicated ROCK endpoint configuration for the WiFi sender
- **AND** it SHALL NOT derive the ROCK target from the auxiliary TCP default server IP used by other modules

#### Scenario: Right-hand STT host is a fixed read-only mirror of the ROCK endpoint
- **WHEN** the right-hand firmware builds its STT proxy URL, at boot or via `va_reload_stt`
- **THEN** its STT host SHALL come from the same `ROCK_SERVER_IP` used by the IMU WiFi sender
- **AND** its STT port SHALL be `8080`
- **AND** changing `ROCK_SERVER_IP` and rebuilding SHALL update both the ROCK IMU uplink and STT proxy host
- **AND** the STT host and port SHALL NOT be independently overridable at runtime — there is no `set_stt_ip` command and `wifi_profile` does not manage an STT endpoint

#### Scenario: Auxiliary TCP remains independently configurable
- **WHEN** the firmware reads its auxiliary TCP server configuration
- **THEN** that endpoint SHALL remain independent from `ROCK_SERVER_IP`
- **AND** changing the PC endpoint SHALL NOT change the ROCK IMU uplink or default STT proxy host

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
- **WHEN** the local endpoint is in `OP_STATE_AUTO_STANDBY`, `OP_STATE_MANUAL_SLEEP`, or `OP_STATE_WAITING_STOP`
- **THEN** the IMU WiFi sender keeps its thread and TCP session available
- **AND** it does not emit IMU CSV frames until the state returns to running

### Requirement: ROCK Downlink Command Handling Over WiFi
Each ART-Pi2 endpoint SHALL accept WiFi/TCP downlink command text from ROCK and apply the same operation control words used by the current firmware state machine.

#### Scenario: WiFi command stream accepts operation-mode control words
- **WHEN** the endpoint receives a complete trimmed TCP line exactly equal to `CMD:RESET_SEQ`, `CMD:START`, or `CMD:STOP`
- **THEN** it forwards the recognized command to the existing operation-mode command handler
- **AND** the resulting state transitions and frame sequence reset behavior match the local firmware operation-mode implementation

#### Scenario: WiFi command stream accepts remote mode-selection words
- **WHEN** the endpoint receives a complete trimmed TCP line exactly equal to `MODE:MANUAL` or `MODE:AUTO`
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

#### Scenario: Initial connection failure triggers delayed retry
- **WHEN** the IMU WiFi sender cannot create or connect its TCP socket
- **THEN** it closes the failed socket when one exists
- **AND** it waits approximately 3 seconds before retrying
- **AND** it continues retrying until the link is restored or the sender is stopped

#### Scenario: Established session failure triggers immediate reconnect attempt
- **WHEN** an established ROCK session reports a send short-write, send error, non-timeout receive error, or peer loss detected by the next send
- **THEN** the sender closes the failed socket
- **AND** it immediately returns to the outer socket/connect loop without an explicit 3-second delay
- **AND** a subsequent failed `connect()` attempt uses the normal 3-second retry delay
