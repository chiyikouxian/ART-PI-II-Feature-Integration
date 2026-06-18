## 1. Contract Definition
- [x] 1.1 Define BLE frame-level parsing requirements for ROCK consumer.
- [x] 1.2 Define per-hand routing rules based on hand_type field.
- [x] 1.3 Define frame_seq alignment and overflow-handling expectations.
- [x] 1.4 Define timestamp fallback expectations.
- [x] 1.5 Define linear-interpolation expectations for missing frames.

## 2. Validation
- [x] 2.1 Validate the OpenSpec change with openspec validate --strict --no-interactive.
- [x] 2.2 Obtain proposal review and approval before any ROCK-side implementation starts.

## 3. ROCK-Side Implementation (Tracked Externally)
- [x] 3.1 ROCK consumer parses [DATA] prefix and 72-field frames per this contract.
- [x] 3.2 ROCK consumer maintains independent per-hand buffers and aligns by frame_seq.
- [x] 3.3 ROCK consumer applies timestamp fallback and linear interpolation per this contract.
- [x] 3.4 ROCK consumer joins-test against live ART-Pi2 firmware.
