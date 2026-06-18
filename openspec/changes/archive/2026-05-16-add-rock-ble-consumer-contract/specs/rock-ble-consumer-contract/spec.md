## ADDED Requirements

### Requirement: BLE Frame Parsing
The ROCK BLE consumer SHALL parse each received BLE notify frame as a UTF-8 CSV text frame matching the ART-Pi2 BLE IMU Uplink Payload Contract, before attempting any numeric conversion.

#### Scenario: [DATA] prefix is stripped before tokenization
- **WHEN** the ROCK consumer receives one BLE notify frame ending in `\n`
- **THEN** the consumer SHALL recognize and strip the literal `[DATA]` prefix at the start of the frame
- **AND** the consumer SHALL NOT include the `[DATA]` prefix in any downstream numeric parsing
- **AND** frames not starting with `[DATA]` SHALL be discarded with a warning log

#### Scenario: Frame is rejected when total field count is not 72
- **WHEN** the ROCK consumer tokenizes a frame by `,`
- **THEN** the consumer SHALL verify the field count equals exactly 72 (1 timestamp + 1 hand_type + 1 frame_seq + 60 ch0-9 ints + 9 ch10 ints)
- **AND** frames with any other field count SHALL be discarded with a warning log

#### Scenario: hand_type and frame_seq fields are extracted as non-numeric tokens
- **WHEN** the ROCK consumer tokenizes a valid 72-field frame
- **THEN** the second field SHALL be parsed as a string and SHALL be either `left` or `right`
- **AND** the third field SHALL be parsed as an unsigned 32-bit integer representing `frame_seq`
- **AND** the remaining 69 fields SHALL be parsed as signed integers (raw IMU readings)

### Requirement: Per-Hand Routing
The ROCK BLE consumer SHALL route each parsed frame into an independent buffer based on the hand_type field, regardless of which BLE adapter or device address delivered the frame.

#### Scenario: Frame with hand_type left is routed to the left-hand buffer
- **WHEN** the ROCK consumer parses a valid frame with hand_type equal to `left`
- **THEN** the frame SHALL be appended to the left-hand buffer
- **AND** the frame SHALL NOT be appended to the right-hand buffer

#### Scenario: Frame with hand_type right is routed to the right-hand buffer
- **WHEN** the ROCK consumer parses a valid frame with hand_type equal to `right`
- **THEN** the frame SHALL be appended to the right-hand buffer
- **AND** the frame SHALL NOT be appended to the left-hand buffer

#### Scenario: Mismatch between BLE source and hand_type is logged
- **WHEN** a frame received from the BLE adapter expecting left-hand traffic carries hand_type equal to `right` (or vice versa)
- **THEN** the consumer SHALL log a warning with both the BLE adapter id and the payload hand_type
- **AND** the consumer SHALL still route the frame according to the payload hand_type value

### Requirement: Frame Sequence Alignment
The ROCK BLE consumer SHALL use the frame_seq field as the primary alignment key when joining left-hand and right-hand frames into a paired sample for downstream inference.

#### Scenario: Left and right frames with matching frame_seq are paired
- **WHEN** both the left-hand buffer and the right-hand buffer contain a frame with the same frame_seq value
- **THEN** those two frames SHALL be paired into a single inference input sample
- **AND** both frames SHALL be removed from their respective buffers after pairing

#### Scenario: frame_seq overflow wraps at uint32 boundary
- **WHEN** the consumer observes frame_seq transition from a near-maximum uint32 value back to a small value
- **THEN** the consumer SHALL treat the wrap as a continuation, not as a fault or restart
- **AND** the alignment logic SHALL continue pairing frames across the wrap boundary

### Requirement: Timestamp Fallback
The ROCK BLE consumer SHALL fall back to the per-frame timestamp_ms when frame_seq-based alignment cannot be performed reliably.

#### Scenario: Duplicate frame_seq triggers timestamp-based alignment
- **WHEN** the consumer receives more than one frame with the same frame_seq from the same hand within a short window
- **THEN** the consumer SHALL fall back to nearest-neighbor alignment using timestamp_ms rather than frame_seq
- **AND** the consumer SHALL log a warning naming the affected hand and the duplicated frame_seq

#### Scenario: Severely out-of-order frame_seq triggers timestamp-based alignment
- **WHEN** the consumer observes frame_seq values arriving in an order that cannot be reconciled by simple wrap-around handling
- **THEN** the consumer SHALL fall back to timestamp_ms-based nearest-neighbor alignment for affected frames

### Requirement: Linear Interpolation For Missing Frames
The ROCK BLE consumer SHALL fill missing frames within a contiguous sequence by linear interpolation across IMU sample fields, so that downstream inference receives a continuous input stream.

#### Scenario: A single missing frame_seq is filled by linear interpolation
- **WHEN** the consumer detects a single gap in frame_seq for one hand (frame N missing, frames N-1 and N+1 present)
- **THEN** the consumer SHALL synthesize the missing frame's IMU integer fields by linear interpolation between frame N-1 and frame N+1
- **AND** the synthesized frame SHALL be marked as interpolated in the consumer's internal state for traceability
- **AND** the synthesized frame's frame_seq SHALL equal N

#### Scenario: Interpolation algorithm parameters are owned by the ROCK implementer
- **WHEN** the ROCK implementer selects window sizes, neighbor selection rules, or numerical methods for interpolation
- **THEN** those choices SHALL be considered implementation details outside the scope of this contract
- **AND** the contract only requires that gaps be filled and that the resulting stream remain continuous for inference

### Requirement: Independent Per-Hand Buffers
The ROCK BLE consumer SHALL maintain two independent buffers, one for each hand, so that loss or delay on one hand does not block frames from the other hand.

#### Scenario: Left-hand buffer continues to accept frames during right-hand stall
- **WHEN** the right-hand BLE link stops delivering frames for a short period
- **THEN** the left-hand buffer SHALL continue to accept and timestamp incoming left-hand frames
- **AND** alignment with the right-hand buffer SHALL resume once right-hand frames return, applying the alignment and gap-handling rules above
