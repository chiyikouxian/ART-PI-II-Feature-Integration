## ADDED Requirements

### Requirement: Per-Hand Operation Mode Selection
Each ART-Pi2 endpoint SHALL maintain its own local operation mode, with automatic mode as the boot default and a local button used to switch that endpoint between automatic mode and manual mode without changing the other hand.

#### Scenario: Endpoint boots into automatic standby
- **WHEN** either ART-Pi2 endpoint starts after power-on or firmware reset
- **THEN** the endpoint SHALL enter automatic mode
- **AND** the endpoint SHALL enter automatic standby within automatic mode
- **AND** the endpoint SHALL keep the current active network path available for later ROCK command/control use
- **AND** the endpoint SHALL NOT persist operation mode changes across power cycles

#### Scenario: Local button switches only the local endpoint mode
- **WHEN** the local mode-switch button is activated on one ART-Pi2 endpoint
- **THEN** that endpoint SHALL switch between automatic mode and manual mode
- **AND** the other ART-Pi2 endpoint SHALL NOT change mode because of this local button event
- **AND** the button event SHALL NOT require direct left-hand/right-hand communication

#### Scenario: ROCK can switch local endpoint mode remotely
- **WHEN** ROCK writes the exact UTF-8 text command `MODE:MANUAL` or `MODE:AUTO` on the current active WiFi/TCP command path
- **THEN** only the addressed endpoint SHALL change its local mode
- **AND** `MODE:MANUAL` SHALL place that endpoint into manual mode and manual sleep
- **AND** `MODE:AUTO` SHALL place that endpoint into automatic mode and automatic standby
- **AND** the other endpoint SHALL NOT change mode unless ROCK sends it a separate command

### Requirement: Active Stream State Control
The operation mode state machine SHALL control the current active IMU stream gate and the current active stream `frame_seq` reset behavior. In the current project stage, the active stream path is WiFi/TCP. BLE code may remain in the repository but is outside the current runtime contract.

#### Scenario: Non-running state keeps the active stream silent
- **WHEN** an endpoint is in a state that disables IMU stream output
- **THEN** the endpoint SHALL NOT emit IMU payloads on the active WiFi/TCP stream path
- **AND** the endpoint SHALL keep the underlying WiFi/TCP client logic available so ROCK commands can still be received when that path is connected

#### Scenario: Running state uses the existing WiFi CSV payload contract
- **WHEN** an endpoint is in running state and the WiFi/TCP session is connected
- **THEN** the endpoint SHALL emit IMU CSV payloads every 90 ms on the active WiFi/TCP stream path
- **AND** each payload SHALL use the existing ART-Pi2 CSV payload format
- **AND** the endpoint SHALL NOT change the field order or raw-value semantics because of operation mode state

### Requirement: Manual Mode ROCK-Controlled Operation
In manual mode, each ART-Pi2 endpoint SHALL remain locally connected to the active control path but stream silent until ROCK writes `CMD:START`; ROCK SHALL stop stream transmission by writing `CMD:STOP`.

#### Scenario: Manual sleep keeps the active path available without IMU streaming
- **WHEN** an endpoint is in manual mode and has not received `CMD:START`
- **THEN** the endpoint SHALL be in manual sleep
- **AND** the endpoint SHALL keep the active WiFi/TCP command path available
- **AND** the endpoint SHALL NOT emit IMU stream payloads
- **AND** the shared IMU acquisition thread SHALL continue updating sensor state at its normal cadence
- **AND** the operation-mode worker SHALL NOT run automatic posture detection while the endpoint is in manual sleep
- **AND** the endpoint SHALL NOT perform automatic special-action wake-up while in manual mode

#### Scenario: ROCK starts manual running
- **WHEN** ROCK writes the exact UTF-8 text command `CMD:START` on the current active command path
- **THEN** the endpoint SHALL reset its active stream `frame_seq` counter to 0
- **AND** the endpoint SHALL enter running state
- **AND** the endpoint SHALL emit IMU stream payloads at the 90 ms running cadence while the active transport is connected

#### Scenario: ROCK stops manual running
- **WHEN** ROCK writes the exact UTF-8 text command `CMD:STOP` on the current active command path while the endpoint is in manual mode
- **THEN** the endpoint SHALL stop IMU stream output
- **AND** the endpoint SHALL reset its active stream `frame_seq` counter to 0
- **AND** the endpoint SHALL enter manual sleep

### Requirement: Automatic Mode Local Wake And ROCK Confirmation
In automatic mode, each ART-Pi2 endpoint SHALL keep the active stream silent during automatic standby, run a local coarse special-action detector every 200 ms, and enter running state when its local detector passes 5 consecutive checks. In the default `auto_protocol=artpi` ROCK flow, the first available paired samples are treated as an ART-Pi START candidate without a second-stage posture rejection pass. The separate `auto_protocol=rock` flow MAY use the ROCK-side sliding-window checker as its own trigger source.

#### Scenario: Automatic standby checks local special action without active streaming
- **WHEN** an endpoint is in automatic standby
- **THEN** the endpoint SHALL keep the active control path available
- **AND** the endpoint SHALL NOT emit IMU stream payloads
- **AND** the endpoint SHALL read or inspect local IMU state every 200 ms
- **AND** the endpoint SHALL evaluate only its local special-action detection result

#### Scenario: Local special action starts running before ROCK confirmation
- **WHEN** an endpoint in automatic standby detects its local special action
- **THEN** the endpoint SHALL enter running state
- **AND** the endpoint SHALL emit IMU stream payloads at the 90 ms running cadence while the active transport is connected
- **AND** the default `auto_protocol=artpi` ROCK flow SHALL treat available paired samples as a START candidate and send `CMD:START` to both endpoints

#### Scenario: First automatic-mode implementation uses only local sensor data from the same hand
- **WHEN** the first automatic-mode implementation evaluates the local special action
- **THEN** it SHALL use only that endpoint's own local sensor channels
- **AND** it SHALL NOT require live sensor data from the other hand before entering running state

#### Scenario: First automatic-mode implementation uses hand-specific posture gates
- **WHEN** the first automatic-mode implementation evaluates the local special action
- **THEN** both endpoints SHALL evaluate dorsal-hand posture and finger posture
- **AND** the left-hand endpoint SHALL also enforce the configured low-motion gyroscope requirement
- **AND** the right-hand endpoint SHALL log gyroscope values above the configured stillness threshold but intentionally bypass that threshold as a blocking gate
- **AND** it SHALL use configurable compile-time thresholds rather than learned classification
- **AND** it SHALL require the pose to satisfy 5 consecutive 200 ms checks before entering running state

#### Scenario: First automatic-mode implementation uses per-hand gates and a minimum finger-pass threshold
- **WHEN** the first automatic-mode implementation evaluates the local special action
- **THEN** the dorsal hand channel SHALL satisfy its local posture window on both endpoints
- **AND** the left-hand local stillness rule SHALL satisfy its gyroscope threshold
- **AND** the right-hand local stillness result SHALL be treated as passing regardless of whether its diagnostic threshold is exceeded
- **AND** the endpoint SHALL count how many of the 10 finger MPU6050 channels satisfy their configured posture windows
- **AND** the endpoint SHALL accept the finger gate when at least the configured minimum count passes
- **AND** the current firmware baseline SHALL use a minimum finger-pass threshold of 6 out of 10 channels
- **AND** both endpoints SHALL clear the consecutive-hit counter whenever the dorsal or finger-count gate fails
- **AND** the left-hand endpoint SHALL additionally clear the counter when its stillness gate fails

#### Scenario: Each hand enters automatic running independently
- **WHEN** the left-hand endpoint detects its local hand-down pose but the right-hand endpoint does not, or vice versa
- **THEN** the detecting endpoint SHALL still enter running state independently
- **AND** the other endpoint SHALL remain in automatic standby until its own local detection passes or ROCK issues commands

#### Scenario: Local coarse wake-up has been verified on both hands
- **WHEN** either endpoint is loaded with firmware that includes the current automatic-mode coarse detector and hand-tuned thresholds
- **THEN** that endpoint SHALL be capable of accumulating 5 consecutive local hits and entering `RUNNING` from `AUTO_STANDBY` using only its own local target-pose data
- **AND** this local verification SHALL NOT by itself imply that ROCK-side second-stage acceptance / rejection has been completed

#### Scenario: Default ART-Pi automatic protocol starts without second-stage rejection
- **WHEN** the orchestrator runs with `auto_protocol=artpi` and receives paired samples after local ART-Pi wake-up
- **THEN** ROCK SHALL treat the presence of those samples as a START candidate
- **AND** the default path SHALL NOT invoke `SpecialActionChecker` as a post-local-wake acceptance or rejection gate
- **AND** the default path SHALL NOT automatically send a rejection command when the optional tighter posture rule would fail

#### Scenario: Optional ROCK automatic protocol uses the sliding-window checker
- **WHEN** the orchestrator runs with `auto_protocol=rock`
- **THEN** `SpecialActionChecker` SHALL review a multi-frame window
- **AND** it SHALL use ART-Pi-provided posture templates with shrunken numeric windows
- **AND** it SHALL apply pass-ratio and dorsal/finger variance limits before using the ROCK-side result as a trigger
- **AND** this ROCK-driven trigger mode SHALL remain distinct from the default ART-Pi-local-wake protocol

#### Scenario: ROCK resets frame sequence without stopping automatic running
- **WHEN** ROCK writes the exact UTF-8 text command `CMD:RESET_SEQ` on the current active command path while the endpoint is in running state
- **THEN** the endpoint SHALL reset its active stream `frame_seq` counter to 0
- **AND** the endpoint SHALL continue IMU stream transmission without entering standby or sleep

#### Scenario: ROCK confirms automatic start without stopping streaming
- **WHEN** ROCK writes the exact UTF-8 text command `CMD:START` on the current active command path while the endpoint is already running in automatic mode
- **THEN** the endpoint SHALL reset its active stream `frame_seq` counter to 0
- **AND** the endpoint SHALL remain in running state
- **AND** the endpoint SHALL continue IMU stream transmission

#### Scenario: ROCK explicitly stops automatic running
- **WHEN** ROCK writes the exact UTF-8 text command `CMD:STOP` on the current active command path while the endpoint is in automatic mode
- **THEN** the endpoint SHALL stop IMU stream output
- **AND** the endpoint SHALL reset its active stream `frame_seq` counter to 0
- **AND** the endpoint SHALL enter automatic standby

#### Scenario: Automatic running does not auto-exit on local pose loss in the first version
- **WHEN** an endpoint has already entered running state through local automatic detection and the local hand-down pose is no longer present
- **THEN** the endpoint SHALL remain in running state
- **AND** local loss of the pose SHALL NOT by itself force a transition back to automatic standby
- **AND** ROCK `CMD:STOP` remains the first-version exit path

#### Scenario: ROCK-started automatic collection can enter waiting-stop after local sign completion
- **WHEN** an endpoint is already in automatic running state
- **AND** ROCK has explicitly started the collection phase on that endpoint
- **AND** the local coarse rule later detects the configured return-to-posture condition for sign completion
- **THEN** the endpoint SHALL send a `WAITING_STOP:<hand>\n` auxiliary text notice on the active WiFi/TCP path
- **AND** the endpoint SHALL enter `OP_STATE_WAITING_STOP`
- **AND** the endpoint SHALL suppress additional local posture-trigger accumulation until translated output handling releases it

#### Scenario: SAY output releases waiting-stop after cooldown
- **WHEN** an endpoint is in `OP_STATE_WAITING_STOP` or its automatic detector is suppressed after sign completion
- **AND** that endpoint receives a `SAY:` translated-text payload on the active WiFi/TCP command path
- **THEN** the endpoint SHALL begin a cooldown window before resuming local posture detection
- **AND** after the cooldown it SHALL return to automatic standby
- **AND** it SHALL clear the local automatic hit counter before new detection begins

#### Scenario: Explicit stop returns the endpoint to a clean automatic retry state
- **WHEN** ROCK writes `CMD:STOP` after an automatic-mode collection attempt
- **THEN** the endpoint SHALL return to automatic standby
- **AND** the endpoint SHALL clear the automatic local consecutive-hit counter
- **AND** the endpoint SHALL reset the active WiFi stream `frame_seq` to 0
- **AND** the endpoint SHALL require a fresh run of 5 consecutive local hits before streaming can begin again

### Requirement: Command Scope Compatibility
Operation-mode commands SHALL use the exact `CMD:` or `MODE:` prefixes and SHALL be distinguished from any non-command translated text payload carried on the same transport.

#### Scenario: Operation commands use the active command path
- **WHEN** ROCK controls an ART-Pi2 endpoint operation state
- **THEN** ROCK SHALL write one of `CMD:RESET_SEQ`, `CMD:START`, `CMD:STOP`, `MODE:MANUAL`, or `MODE:AUTO` on the current active command path
- **AND** the endpoint SHALL treat the command as operation control rather than translated speech text

#### Scenario: SAY-prefixed text remains available for translated-text behavior
- **WHEN** the left-hand endpoint receives a complete line beginning with `SAY:` or `say:`
- **THEN** the payload after that prefix SHALL remain available to the existing translated-text handling path
- **AND** arbitrary unprefixed text SHALL be logged as an unknown command and ignored
- **AND** operation-mode command handling SHALL NOT reinterpret SAY text as a mode transition

### Requirement: Direct Hand-To-Hand Communication Exclusion
The dual-hand operation modes SHALL NOT require direct communication between the left-hand ART-Pi2 endpoint and the right-hand ART-Pi2 endpoint.

#### Scenario: ROCK coordinates both hands
- **WHEN** the system needs both endpoints to reset frame sequence, start running, or stop running
- **THEN** ROCK SHALL write the corresponding command to each endpoint independently
- **AND** each endpoint SHALL act only on its own local button state, local special-action detection, and ROCK commands
- **AND** neither endpoint SHALL be required to communicate directly with the other endpoint
