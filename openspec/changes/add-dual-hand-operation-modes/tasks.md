## 1. Specification
- [x] 1.1 Define per-hand operation mode states for automatic and manual modes.
- [x] 1.2 Define ROCK command words and frame sequence reset behavior.
- [x] 1.3 Define local button mode switching as per-hand and non-persistent.
- [x] 1.4 Define that the current active stream path is WiFi/TCP, while BLE code remains deferred in the current runtime contract.

## 2. Manual Mode Implementation First
- [x] 2.1 Add shared per-hand operation state APIs used by the current sender and command handling code.
- [x] 2.2 Add `CMD:START`, `CMD:STOP`, and `CMD:RESET_SEQ` parsing on the current active command path on both hands.
- [x] 2.3 Gate active IMU stream output by operation state without changing the current CSV payload format.
- [x] 2.4 Reset the per-hand active WiFi `frame_seq` on `CMD:RESET_SEQ`, `CMD:START`, and `CMD:STOP` through a formal public reset path instead of only resetting the BLE-side counter.
- [x] 2.5 Add per-hand local button switching between automatic and manual modes without persistent storage.
- [x] 2.6 Add ROCK-side `MODE:MANUAL` and `MODE:AUTO` remote mode-selection commands on the current WiFi/TCP command path.
- [ ] 2.7 Validate manual mode on hardware: boot, switch to manual sleep, ROCK `CMD:START`, 90 ms WiFi stream, ROCK `CMD:STOP`, stop stream, frame sequence reset.

## 3. Automatic Mode Implementation Second
- [x] 3.1 Change the automatic-standby worker cadence inside `operation_mode` from 500 ms to 200 ms.
- [x] 3.2 Expand first-version coarse special-action detection from dorsal-only to dorsal-hand plus local finger IMU data.
- [x] 3.3 Add configurable local posture thresholds for hand-down posture, finger posture, and local stillness.
- [x] 3.4 Require 5 consecutive 200 ms local hits before entering running state.
- [x] 3.5 Keep the transition owner inside `operation_mode` so automatic standby can enter running when the local coarse rule passes.
- [x] 3.6 Preserve 90 ms WiFi stream behavior in running state after automatic wake-up.
- [x] 3.7 Keep automatic running latched until ROCK writes `CMD:STOP`; do not auto-exit on local pose loss.
- [x] 3.8 Add standby-stage debug logging or equivalent observability needed for local coarse-threshold tuning.
- [x] 3.9 Extend `auto_status` or equivalent tooling so dorsal and finger channels needed by the coarse rule can be inspected during tuning.
- [x] 3.10 Capture left-hand and right-hand local target-pose samples and tune the ART-Pi thresholds enough for both hands to enter `RUNNING` locally in hardware.
- [ ] 3.10b Capture a broader non-target-pose dataset and tighten thresholds against false wake-up risk.
- [ ] 3.11 Implement ROCK-side second-stage sliding-window review using tighter thresholds and fluctuation checks.
- [x] 3.12 Add ROCK rejection handling that returns the endpoint to standby and clears both local hit count and active WiFi stream `frame_seq`.
- [ ] 3.13 Validate the full automatic workflow on hardware: independent local coarse wake-up, ROCK second-stage acceptance/rejection, clean retry after rejection, and explicit stop/reset commands.
- [x] 3.14 Validate the local automatic workflow on hardware: both left-hand and right-hand endpoints can independently accumulate 5 hits and enter `RUNNING`.
- [x] 3.15 Add `WAITING_STOP` plus `SAY`-driven cooldown handling so automatic-mode collection can pause posture detection after local sign completion.
- [x] 3.16 Add non-persistent runtime threshold/model text uplink so current local window values can be reported to ROCK and re-sent after mode changes.
- [x] 3.17 Add local calibration-thread support for runtime threshold refresh in the current firmware branch.

## 4. Validation
- [ ] 4.1 Re-run `openspec validate add-dual-hand-operation-modes --strict --no-interactive` after the WiFi-aligned wording update.
- [x] 4.2 Review the updated spec against the current `main.c` reality that BLE runtime init remains disabled.
