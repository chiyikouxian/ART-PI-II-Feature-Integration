## ADDED Requirements

### Requirement: BLE CSV Fragment Notify Format
When a complete IMU CSV frame exceeds the safe BLE Notify payload size, the ART-Pi2 firmware SHALL split the frame into fragments, each sent as a separate BLE notification with the format `[FRAG]<frame_seq>,<hand_type>,<frag_idx>,<frag_total>,<payload_part>`. Each fragment notification SHALL NOT exceed 180 bytes total.

#### Scenario: Fragment format contains required fields
- **WHEN** an IMU CSV frame is fragmented into N parts
- **THEN** each fragment SHALL begin with the literal prefix `[FRAG]`
- **AND** the header SHALL contain frame_seq (uint32), hand_type (left/right), frag_idx (0-based uint16), and frag_total (uint16)
- **AND** the header fields SHALL be separated by commas
- **AND** the payload_part SHALL follow the fourth comma and contain a slice of the complete [DATA] CSV bytes

#### Scenario: Fragment total size constraint
- **WHEN** a fragment is constructed for BLE notification
- **THEN** its total length in bytes SHALL be <= 180
- **AND** the payload_part maximum length SHALL be computed as 180 minus the header length

### Requirement: Fragment Frame Sequence Semantics
All fragments of a single complete IMU CSV frame SHALL carry the same frame_seq value. The frame_seq SHALL be incremented exactly once after all fragments of a frame have been sent, regardless of individual fragment send results.

#### Scenario: Same frame_seq across fragments
- **WHEN** a complete IMU CSV frame requires 3 fragments
- **THEN** all 3 fragments SHALL use the same frame_seq value
- **AND** frag_idx SHALL be 0, 1, 2 respectively
- **AND** frag_total SHALL be 3 for all fragments

#### Scenario: frame_seq increments after all fragments
- **WHEN** all fragments for a frame have been sent (or attempted)
- **THEN** frame_seq SHALL be incremented exactly once
- **AND** if no notify was attempted (e.g., mbuf alloc failed on every fragment), frame_seq SHALL NOT be incremented

### Requirement: ROCK Fragment Reassembly
The ROCK bluetooth_service SHALL reassemble [FRAG] notifications into complete [DATA] CSV text before parse_frame(). Reassembly SHALL be keyed by (hand_type, frame_seq). Direct [DATA] text SHALL pass through unchanged.

#### Scenario: Fragments reassembled in order
- **WHEN** all fragments for a given (hand_type, frame_seq) have been received
- **THEN** the payload_parts SHALL be joined in frag_idx order (0, 1, 2, ...)
- **AND** the resulting full_text SHALL begin with the [DATA] prefix
- **AND** the full_text SHALL be returned for parse_frame()

#### Scenario: Direct [DATA] backward compatibility
- **WHEN** a BLE notification text begins with [DATA]
- **THEN** it SHALL be returned as-is without fragmentation processing
- **AND** existing parse_frame() logic SHALL apply unchanged

#### Scenario: Incomplete fragment does not parse
- **WHEN** a [FRAG] notification is received but not all fragments for that key are present
- **THEN** the reassembler SHALL return None
- **AND** no partial parse SHALL be attempted

### Requirement: Fragment Loss Handling
Incomplete fragment sets SHALL be discarded after a timeout to prevent memory growth. A maximum number of pending fragment sets SHALL be enforced.

#### Scenario: Timeout discards incomplete fragments
- **WHEN** the first fragment of a new key arrives
- **THEN** its arrival time SHALL be recorded
- **AND** if all fragments are not received within FRAG_TIMEOUT_SEC (1.0s), the entire pending set SHALL be discarded with a warning log

#### Scenario: Maximum pending frames cap
- **WHEN** the number of pending incomplete fragment sets reaches MAX_PENDING_FRAMES (128)
- **THEN** the oldest pending set SHALL be evicted before adding a new key

### Requirement: Backward Compatibility With Direct DATA Frames
The fragmentation mechanism SHALL NOT break existing behavior for frames that fit within a single BLE notification. Small [DATA] frames SHALL continue to work without fragmentation.

#### Scenario: Small frame not fragmented
- **WHEN** a complete [DATA] CSV frame length is <= 180 bytes
- **THEN** the firmware MAY send it directly as a single [DATA] notification without [FRAG] wrapping
- **AND** the ROCK SHALL process it through parse_frame() unchanged
