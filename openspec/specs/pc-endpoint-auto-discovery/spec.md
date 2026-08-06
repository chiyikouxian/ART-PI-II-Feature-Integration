# pc-endpoint-auto-discovery Specification

## Purpose
Define how the PC advertises its current TCP endpoint, how each ART-Pi2 validates and publishes that endpoint atomically, and how PC-facing TCP clients reconnect without affecting the independent ROCK links.
## Requirements
### Requirement: PC Discovery Broadcast
The PC frontend SHALL advertise a versioned discovery packet on UDP port `9108` after initiating the TCP listener on port `9109`, using limited broadcast, hotspot-directed broadcast, and a `/24` unicast fallback.

#### Scenario: Normal discovery broadcast
- **WHEN** the PC frontend has a routable IPv4 address
- **THEN** it broadcasts `ARTPI_PC,1,9109\n` to `255.255.255.255:9108`
- **AND** it broadcasts the same payload to the selected interface's `/24` directed broadcast address
- **AND** the broadcast socket uses the IPv4 interface selected for the default route

#### Scenario: Hotspot filters all broadcast frames
- **WHEN** the PC has a selected `/24` hotspot address
- **THEN** the frontend sends the discovery payload to each peer address in that `/24` subnet at least once every two seconds
- **AND** no configured ART-Pi2 address is required

#### Scenario: PC address changes
- **WHEN** the selected local IPv4 address changes
- **THEN** the frontend closes the old discovery socket
- **AND** it binds a new socket to the new local IPv4 address
- **AND** it continues broadcasting without restarting the Flask process

### Requirement: Strict Firmware Packet Validation
Each ART-Pi2 endpoint MUST accept only correctly formatted, supported discovery packets and MUST derive the PC IPv4 address from the UDP source address.

#### Scenario: Valid packet received
- **WHEN** a board receives `ARTPI_PC,1,<port>` with optional trailing CR/LF and a decimal port in the range `1..65535`
- **THEN** it uses the `recvfrom()` source IPv4 address and parsed port as the candidate PC endpoint

#### Scenario: Invalid packet received
- **WHEN** the magic, version, separators, numeric fields, port range, or trailing-field validation fails
- **THEN** the board discards the packet
- **AND** it does not update the PC endpoint generation

### Requirement: Atomic Endpoint Publication
The firmware SHALL publish the PC IP, TCP port, and generation as one mutex-protected endpoint state.

#### Scenario: Endpoint changes
- **WHEN** discovery receives an IP or port different from the current endpoint
- **THEN** `server_config` atomically stores the new IP and port
- **AND** it increments the endpoint generation exactly once

#### Scenario: Repeated identical broadcast
- **WHEN** discovery receives the currently configured IP and port again
- **THEN** the update is a no-op
- **AND** the generation does not change

### Requirement: Automatic TCP Reconnection
Each PC-facing TCP client SHALL reconnect using a fresh endpoint snapshot when the endpoint generation changes.

#### Scenario: Hotspot assigns a new PC address
- **WHEN** a running board discovers a different PC IPv4 address
- **THEN** its current PC TCP session exits cleanly
- **AND** it reconnects to the new address on the advertised port
- **AND** no firmware reflash or board reboot is required

#### Scenario: Discovery is temporarily unavailable
- **WHEN** no valid broadcast is received during the initial discovery wait
- **THEN** firmware may start the TCP retry loop with the configured fallback endpoint
- **AND** the background discovery thread continues listening for a later valid broadcast

### Requirement: Discovery Lifecycle and WiFi Recovery
The discovery module MUST maintain a process-lifetime event object and rebuild its UDP socket across WiFi loss without detaching the event during normal stop/start cycles.

#### Scenario: WiFi disconnects and reconnects
- **WHEN** the board loses WiFi readiness
- **THEN** discovery closes its UDP socket and waits
- **AND** after WiFi becomes ready it creates and binds a new UDP `9108` socket

#### Scenario: Discovery is stopped while a caller waits
- **WHEN** `pc_discovery_stop()` is called while `pc_discovery_wait_server()` is blocked
- **THEN** the STOPPED event wakes the caller
- **AND** the wait returns `-RT_ERROR`

#### Scenario: Discovery wait times out
- **WHEN** no valid packet arrives within the requested finite timeout
- **THEN** `pc_discovery_wait_server()` returns `-RT_ETIMEOUT`

### Requirement: ROCK Path Isolation
PC endpoint discovery MUST NOT modify ROCK server addresses, ports, frame sequences, or sessions.

#### Scenario: PC endpoint changes
- **WHEN** discovery updates the PC endpoint and the PC TCP client reconnects
- **THEN** ROCK traffic on ports `9101` and `9102` continues under its existing configuration and lifecycle
