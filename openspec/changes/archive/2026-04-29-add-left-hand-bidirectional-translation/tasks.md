## 1. Proposal Scope
- [x] 1.1 Confirm the left-hand endpoint reuses the current NimBLE-based BLE path to communicate with ROCK.
- [x] 1.2 Confirm the left-hand endpoint reuses the current TCP client path and PC TCP server for all PC uploads.
- [x] 1.3 Confirm no communication-framework changes are introduced by this change.

## 2. Left-Hand Firmware
- [x] 2.1 Verify the left-hand firmware can continuously collect 11-channel IMU data using the existing collection path.
- [x] 2.2 Define the left-hand BLE send path for forwarding IMU data to ROCK.
- [x] 2.3 Define the left-hand TCP upload path for IMU and battery data to the PC.
- [x] 2.4 Define the left-hand BLE receive path for processed text returned from ROCK.
- [x] 2.5 Define the left-hand VTX316 playback trigger for received text.
- [x] 2.6 Define the left-hand TCP upload path for BLE-received translated text as an extension of the existing left-hand JSON payload, not as a separate text-only message shape.

## 3. Integration Expectations
- [x] 3.1 Align the left-hand BLE payload contract with the existing ROCK-side BLE service expectations without introducing a new transport.
- [x] 3.2 Align the left-hand TCP payload contract with the existing PC TCP ingestion flow by preserving the current `device`, `id`, left-hand sensor object, and battery structure while adding translated-text data as an additive field.
- [x] 3.3 Document expected sequencing and failure handling for BLE disconnects, missing translated text, and temporary TCP unavailability.

## 4. Validation
- [x] 4.1 Validate the OpenSpec change with `openspec validate --strict --no-interactive`.
- [x] 4.2 Obtain proposal review and approval before any implementation starts.
