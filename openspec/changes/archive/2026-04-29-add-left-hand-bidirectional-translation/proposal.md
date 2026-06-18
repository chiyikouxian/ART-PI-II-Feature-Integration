# Change: Complete Left-Hand Bidirectional Translation Flow

## Why
The repository already treats the right-hand path as basically complete, while the left-hand path remains the missing side of the intended bidirectional sign language translation system. The project needs a tight proposal that completes the left-hand runtime flow without changing the established communication framework.

## What Changes
- Define the left-hand endpoint behavior for continuous IMU collection and BLE uplink to the ROCK edge device.
- Define the left-hand endpoint behavior for TCP uplink of IMU and battery data to the PC using the existing TCP client path.
- Define the left-hand endpoint behavior for receiving processed text from ROCK over BLE.
- Define the left-hand endpoint behavior for speaking returned text through the existing VTX316 path.
- Define the left-hand endpoint behavior for uploading BLE-received processed text to the PC by extending the existing left-hand TCP JSON payload, rather than sending a separate text-only TCP message shape.
- Keep scope limited to the left-hand endpoint contract and any required PC/ROCK-facing compatibility expectations.
- Explicitly exclude communication-framework changes such as replacing BLE, replacing TCP, or introducing new transports.

## Impact
- Affected specs: `left-hand-bidirectional-translation`
- Affected code:
  - `art_pi2_left/applications/main.c`
  - `art_pi2_left/applications/ble_app.c`
  - `art_pi2_left/applications/imu_ble_service.c`
  - `art_pi2_left/applications/imu_notify_thread.c`
  - `art_pi2_left/applications/tcp_client.c`
  - `art_pi2_left/applications/adc_battery.c`
  - `art_pi2_left/vtx316/vtx316.c`
  - `leading_end/app/services/tcp_server.py`
- External dependencies:
  - ROCK edge device BLE service must remain the peer for left-hand IMU uplink and processed-text downlink.
  - PC TCP server remains the receiver for left-hand IMU, battery, and translated-text uploads.
  - PC-side consumers that currently read `device:"left"` latest-data records remain compatible because the existing left-hand sensor payload shape is preserved and only extended with translated-text data.
