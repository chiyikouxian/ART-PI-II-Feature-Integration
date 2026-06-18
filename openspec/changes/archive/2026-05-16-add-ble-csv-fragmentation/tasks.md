# Tasks: BLE CSV Fragment Sending and ROCK Fragment Reassembly

## 1. Contract

- [x] 1.1 Define [FRAG] BLE notify fragmentation format.
- [x] 1.2 Define same-frame frame_seq and frag_idx/frag_total semantics.
- [x] 1.3 Define ROCK reassembly behavior by hand_type + frame_seq.
- [x] 1.4 Define incomplete-fragment timeout/drop behavior.

## 2. Firmware

- [x] 2.1 Implement left-hand BLE CSV fragmentation.
- [x] 2.2 Implement right-hand BLE CSV fragmentation.
- [x] 2.3 Verify each fragment payload stays within safe Notify size.

## 3. ROCK

- [ ] 3.1 Implement [FRAG] reassembly before parse_frame().
- [ ] 3.2 Preserve direct [DATA] backward compatibility.
- [ ] 3.3 Verify py_compile passes.

## 4. Validation

- [x] 4.1 Run openspec validate add-ble-csv-fragmentation --strict --no-interactive.
- [x] 4.2 Run firmware build/Keil validation externally.
