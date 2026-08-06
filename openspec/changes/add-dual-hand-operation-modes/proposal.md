# Change: Add Dual-Hand Operation Modes

## Why
The two ART-Pi2 endpoints need explicit operating modes so the system can remain quiet until recognition should begin, support a manual ROCK-controlled workflow first, and later add automatic local gesture wake-up. In the current hardware/software stage, BLE and WiFi cannot be kept active together reliably, so the project has retained the WiFi/TCP streaming path as the active runtime path and temporarily disabled BLE runtime transmission while keeping related BLE code in the tree for later reuse.

## What Changes
- Add a dual-hand operation mode contract covering automatic mode and manual mode for both ART-Pi2 endpoints.
- Define the default boot behavior as automatic standby, with non-persistent per-hand button switching between automatic and manual modes.
- Define ROCK downlink commands on the current active WiFi/TCP command path: `CMD:RESET_SEQ`, `CMD:START`, and `CMD:STOP`.
- Define additional ROCK mode-selection commands on the same WiFi/TCP command path: `MODE:MANUAL` and `MODE:AUTO`.
- Define that the state machine currently controls the active WiFi IMU stream gate and active WiFi stream `frame_seq`; BLE-related code is retained but excluded from the current runtime contract.
- Define implementation order so manual mode is implemented and validated before automatic local special-action detection.
- Define the first automatic-mode implementation as a pure-rule local detector with no neural network or learned model; keep the separate ROCK-protocol sliding-window checker as an optional trigger path rather than a default post-local-wake reviewer.
- Define that each hand performs its own local coarse detection using only local sensor data: both hands gate on dorsal-hand and finger posture, the left hand also gates on local stillness, and the right hand intentionally logs but bypasses stillness failure.
- Define that the local detector polls every 200 ms and requires 5 consecutive hits before entering `RUNNING`.
- Define that the default `auto_protocol=artpi` ROCK flow starts from the first available paired samples without a second-stage posture rejection pass, while `auto_protocol=rock` uses its own sliding-window checker as the trigger source.
- Define that explicit `CMD:STOP` returns an endpoint to automatic standby and clears both the local auto-trigger hit counter and the active WiFi `frame_seq`.
- Define the current auto-collection extension in which a post-sign local return-to-posture event moves the endpoint into `WAITING_STOP`, suppresses re-triggering, and waits for `SAY:` plus cooldown before re-enabling standby detection.
- Define a lightweight auxiliary text uplink used by the firmware to send current threshold/model metadata and `WAITING_STOP` notices to ROCK over the existing WiFi/TCP session.

## Impact
- Affected specs: `dual-hand-operation-modes` (new capability)
- Affected code: left/right ART-Pi2 WiFi sender control, ROCK command handling, local mode state, local button handling, local coarse special-action detection logic, runtime threshold/model metadata reporting, and ROCK automatic-protocol orchestration
- Deferred code path: left/right BLE notify and Text Characteristic code remain in the repository but are outside the current runtime contract because BLE init is intentionally disabled in `main.c`
- Non-goals: direct left-hand/right-hand communication between endpoints, BLE/WiFi coexistence work in this change, persistent mode storage, neural-network-based special-action detection, and coupling special-action logic to the hand-sign recognition model
