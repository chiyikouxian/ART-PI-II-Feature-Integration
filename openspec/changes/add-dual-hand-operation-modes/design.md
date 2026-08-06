## Context
The current firmware already supports manual mode state control through local button switching and ROCK text commands. The next planned stage is automatic mode wake-up. The user has now constrained the first automatic implementation:

- Trigger action: both products use a fixed "special action" characterized by hand-down and fist posture. The left-hand local detector also enforces stillness; the right-hand detector intentionally keeps its stillness comparison diagnostic-only and does not block wake-up on that result.
- Detection scope: each hand decides independently whether it should enter `RUNNING`.
- Combined dual-hand semantics: in the default `auto_protocol=artpi` flow, ROCK treats the first available paired samples as the START candidate without a second-stage posture acceptance/rejection pass. The separate `auto_protocol=rock` flow uses the ROCK sliding-window checker as its own trigger source.
- Exit path: the current implementation exits automatic running on explicit `CMD:STOP`, which returns the endpoint to standby and resets the active session state.
- Detection cadence: local coarse detection runs every 200 ms.
- Runtime transport: the current effective transport is WiFi/TCP. BLE code remains in the repository, but BLE runtime init and BLE notify are intentionally disabled because of coexistence conflicts with WiFi in the current stage.

The codebase already exposes all 11 local IMU channels through `mpu_get_channel_raw_data(...)`, which is sufficient for a pure-rule detector without involving any ML model.

## Goals / Non-Goals
- Goals:
  - Add a deterministic first-version automatic wake path that is fully rule-based and tunable on hardware.
  - Document the two implemented automatic protocols separately: ART-Pi local wake as the default source, and the optional ROCK sliding-window checker as an alternative trigger source.
  - Keep the state transition model compatible with current manual-mode behavior and current ROCK command ownership.
  - Align the specification with the current code reality that WiFi/TCP is the active streaming and command transport.
- Non-Goals:
  - Introduce neural-network, classifier, or learned-model logic into the special-action trigger.
  - Require one endpoint to read the other endpoint's local sensor data before entering running state.
  - Implement local automatic exit only from pose disappearance without ROCK involvement.
  - Add persistent calibration storage.
  - Solve the final production threshold tuning inside the design itself.
  - Re-enable BLE/WiFi coexistence in this change.

## Decisions
- Decision: Use a pure-rule detector on both ART-Pi and ROCK; do not use AI for special-action triggering.
  - Why:
    - The target special action is a static, highly constrained posture with prolonged stillness.
    - Deterministic thresholds and fluctuation checks are easier to tune, explain, and reproduce than a trained model.
    - This keeps special-action triggering fully isolated from the hand-sign recognition pipeline.

- Decision: ART-Pi local coarse detection uses only local sensor data from that same hand.
  - Why:
    - The user confirmed that one hand must not depend on the other hand's sensor data before entering `RUNNING`.
    - This preserves the existing per-hand independence model.

- Decision: ART-Pi local coarse detection checks both dorsal-hand posture and finger posture.
  - Why:
    - The special action is no longer treated as only a dorsal orientation test.
    - Using local finger IMUs tightens the trigger meaningfully before streaming begins.

- Decision: The current local coarse detector requires `dorsal + minimum finger-pass count`; the left hand additionally requires stillness, while the right hand intentionally bypasses stillness as a blocking gate.
  - Why:
    - Hardware measurements showed that requiring all 10 finger MPU6050 channels to pass simultaneously was too brittle for the current wearable mounting spread.
    - The current firmware uses `AUTO_FINGER_MIN_PASS = 6`, meaning at least 6 of the 10 finger channels must satisfy their local windows while the dorsal channel passes.
    - The left-hand gyroscope threshold remains part of the pass condition. The right-hand code logs values above its configured threshold but deliberately forces the stillness result to pass, as confirmed by the project owner.

- Decision: Keep detection in `operation_mode` as a dedicated periodic standby worker.
  - Why:
    - The operation-mode state machine already owns `AUTO_STANDBY` versus `RUNNING`.
    - A local worker thread preserves the planned 200 ms cadence cleanly.
    - This avoids pushing automatic-mode logic into unrelated sensing modules.

- Decision: Treat WiFi/TCP as the current active control and streaming path.
  - Why:
    - BLE runtime init is deliberately disabled in `main.c`.
    - `imu_wifi_sender.c` already consumes the `operation_mode` state and ROCK `CMD:*` messages in the live build.
    - The specification should describe the behavior that is actually being built and tested now.

- Decision: Allow ROCK to switch local mode explicitly with `MODE:MANUAL` and `MODE:AUTO`.
  - Why:
    - The current firmware already parses these exact text commands on the active WiFi/TCP command path.
    - This lets ROCK align the endpoint mode with the collection workflow without requiring a local button press.

- Decision: Add a `WAITING_STOP` state and `SAY`-driven cooldown after sign completion in automatic mode.
  - Why:
    - The current firmware now distinguishes "streaming because recognition is active" from "streaming session finished and waiting for translated output".
    - A short suppression window avoids immediate re-triggering while the translated result is being delivered.

- Decision: Report runtime threshold/model metadata to ROCK on the same WiFi/TCP session.
  - Why:
    - The current firmware can now queue `MODEL:*` and `WAITING_STOP:*` sideband text for observability and orchestration.
    - This keeps threshold visibility and phase notifications on the already-active link without introducing another transport.

- Decision: Require 5 consecutive local hits at 200 ms cadence before entering `RUNNING`.
  - Why:
    - A single frame is too sensitive to transient arm swing and sensor noise.
    - `5 x 200 ms = 1.0 s` is short enough to feel responsive while still filtering many accidental poses.

- Decision: Use compile-time tunable thresholds in the first version.
  - Why:
    - Axis polarity and practical pose spread must be confirmed on hardware.
    - Tunable constants allow quick iteration without redesigning the algorithm.
    - The current firmware branch extends this with non-persistent runtime refresh through a local calibration thread and re-sent `MODEL:*` metadata.

- Decision: ROCK exposes two distinct automatic protocols rather than an unconditional two-stage review pipeline.
  - Why:
    - `auto_protocol=artpi` is the default and directly treats available paired samples after local wake-up as the START candidate.
    - `auto_protocol=rock` runs `SpecialActionChecker` over a multi-frame window and uses a passing window as its own START trigger.
    - The current code does not invoke `SpecialActionChecker` as a post-local-wake rejection gate in the default ART-Pi protocol.

- Decision: Explicit `CMD:STOP` clears both the local trigger hit counter and the active stream frame sequence.
  - Why:
    - Clearing both gives an explicit session boundary before the endpoint returns to standby.
    - The default ART-Pi protocol does not currently generate an automatic posture-rejection command.

## Detection Algorithm (First Version)
### ART-Pi local coarse detector
The first version should evaluate the following in `AUTO_STANDBY` every 200 ms:

1. Read local IMU data for the dorsal hand sensor and local finger sensors.
2. Reject the sample if any required channel for the coarse rule is unavailable.
3. Check whether the dorsal hand posture lies inside the configurable "hand-down" orientation window.
4. Check whether the local finger posture lies inside configurable "fist/closed" windows.
5. Count how many finger channels pass their local windows and require at least the configured minimum pass count.
6. On the left hand, require the configured gyroscope stillness bounds. On the right hand, log threshold exceedance for diagnostics but intentionally treat the stillness result as passing.
7. If the sample matches, increment a consecutive-hit counter up to a small cap.
8. If the sample fails, clear the consecutive-hit counter.
9. Enter `RUNNING` only when the counter reaches `AUTO_WAKE_CONSECUTIVE_HITS = 5`.

### ROCK automatic protocol branches
In the default `auto_protocol=artpi` branch, once ART-Pi starts streaming and paired samples are available, ROCK uses that pair as the START candidate and sends `CMD:START` to both endpoints. It does not run the sliding-window checker as a post-local-wake acceptance/rejection stage.

In the separate `auto_protocol=rock` branch, ROCK uses `SpecialActionChecker` as its own trigger source:

1. Buffer a sliding window of roughly 20-30 frames for each hand.
2. Re-evaluate the same semantic rule family:
   - dorsal hand posture
   - local finger posture
   - low gyroscope motion
   - left/right symmetry across the window
3. Tighten the accepted numeric windows relative to ART-Pi.
4. Compute fluctuation metrics such as variance or spread inside the window to reject unstable "almost correct" poses.
5. Produce a START candidate only if the whole window is stable enough.

Adding a post-local-wake checker that automatically rejects the default ART-Pi flow remains deferred work and is not part of current behavior.

## Tunable Constants
The first implementation should define constants similar to:

- `AUTO_CHECK_INTERVAL_MS = 200`
- `AUTO_WAKE_CONSECUTIVE_HITS = 5`
- dorsal orientation window bounds
- finger posture window bounds
- minimum passing finger-channel count
- local gyroscope stillness bounds
- ROCK-side tighter posture windows
- ROCK-side variance / fluctuation limits

The exact numeric windows remain tunable and may continue to change with hardware capture, but the current code path has already been verified on hardware to allow both hands to enter `RUNNING` locally using target-pose samples.

## Recommended Rollout
1. Keep `operation_mode` as the single local state owner for standby versus running.
2. Add the 200 ms standby detection worker and state-transition plumbing with conservative local thresholds.
3. Add debug logging / status commands that expose dorsal and finger raw data while in automatic standby.
4. Add an explicit WiFi sender `frame_seq` reset entry point so command-driven state transitions can reset the active stream consistently.
5. Capture target-pose samples for left and right hands separately and tune local coarse thresholds until each hand can independently enter `RUNNING`.
6. Capture broader non-target-pose samples and tighten thresholds only as much as needed to reduce false wake-up risk without breaking local wake-up.
7. Keep the existing ROCK-protocol sliding-window trigger covered independently from the default ART-Pi protocol.
8. If a post-local-wake acceptance/rejection stage is added later, specify its command and state-transition contract before enabling it in the default flow.
9. Validate the new `WAITING_STOP -> SAY -> cooldown -> AUTO_STANDBY` path and the remote `MODE:*` command path against the live WiFi/TCP session.

## Risks / Trade-offs
- Risk: Wrong dorsal-axis or finger-axis assumption for the target special action.
  - Mitigation:
    - Keep thresholds configurable.
    - Add temporary logs during standby tuning.

- Risk: False wake-up from ordinary static poses if ART-Pi coarse thresholds are too loose, especially because the right-hand stillness failure is intentionally bypassed.
  - Mitigation:
    - Require 5 consecutive hits at 200 ms.
    - Include both hand posture and finger posture gates.
    - Capture broader non-target-pose data and tune the implemented local gates without changing the intentional right-hand bypass.

- Risk: 200 ms cadence increases local polling load.
  - Mitigation:
    - Keep the local rule lightweight and deterministic.
    - Revisit only if measured CPU or power cost becomes problematic.

- Risk: BLE code comments and historical interfaces may mislead future work while BLE runtime remains disabled.
  - Mitigation:
    - Keep the spec explicit that BLE is deferred in the current stage.
    - Prefer WiFi-path verification as the current source of truth.

## Open Questions
- The current left-hand and right-hand numeric windows are still considered field-tunable and need broader non-target-pose coverage before being treated as stable production thresholds.
- The current runtime calibration thread updates thresholds only in RAM and does not persist them across reset.
- A ROCK-side post-local-wake acceptance/rejection stage for the default `auto_protocol=artpi` flow is not implemented. The existing `auto_protocol=rock` sliding-window checker is an alternative trigger path, not that deferred reviewer.
- If BLE/WiFi coexistence is solved later, the operation-mode contract will need a follow-up change that reintroduces BLE as an active runtime path rather than only a retained code path.
