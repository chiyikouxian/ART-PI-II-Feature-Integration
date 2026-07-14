## Context
The PC and both ART-Pi2 boards join the same phone hotspot. The hotspot DHCP server may assign a new PC IPv4 address after reconnection. A fixed firmware address is therefore not stable enough for repeated field testing.

## Goals / Non-Goals
- Goals:
  - Discover the current PC address without reflashing either board.
  - Reconnect an already-running TCP client when the discovered endpoint changes.
  - Keep endpoint reads and writes race-free across discovery and TCP threads.
  - Recover automatically across WiFi loss and hotspot restart.
- Non-Goals:
  - Discover or modify ROCK endpoints.
  - Provide authentication or encryption for the local test network.
  - Replace DHCP, mDNS, DNS, or production service discovery.
  - Claim hardware validation before the hotspot-change test is completed.

## Decisions
- Decision: PC broadcasts `ARTPI_PC,1,9109\n` once per second to both `255.255.255.255:9108` and the selected interface's `/24` directed broadcast address.
  - The payload is small, versioned, human-readable, and requires no third-party Python package.
  - The directed broadcast is required because the tested Windows phone-hotspot path permits unicast peers but does not forward the limited broadcast.
- Decision: PC additionally performs a `/24` unicast discovery sweep every two seconds.
  - Hardware testing confirmed that the active phone hotspot filters both limited and directed broadcast while permitting client-to-client unicast.
  - A `/24` sweep sends at most 253 small UDP datagrams every two seconds and requires no board address configuration or response protocol.
- Decision: Firmware trusts the IPv4 source address returned by `recvfrom()`, not an IP embedded in the payload.
  - This prevents a malformed payload from injecting an arbitrary endpoint address.
- Decision: The parser strictly validates the eight-byte magic, separators, decimal-only version and port, port range, and absence of trailing fields.
- Decision: `server_config` owns a mutex-protected endpoint and generation counter.
  - Discovery atomically updates IP and port; TCP obtains one consistent snapshot.
  - Repeated broadcasts with the same endpoint are no-ops and do not force reconnects.
- Decision: TCP compares the connected generation during its normal loop.
  - A generation change closes the current PC socket and reconnects using a fresh snapshot.
- Decision: The discovery event is initialized once by `pc_discovery_init()` and remains alive for the firmware lifetime.
  - Start resets pending event bits; stop sends a STOPPED event and never detaches the object.
- Decision: The Python broadcaster binds its socket to the IPv4 address selected by the default route.
  - This avoids accidentally broadcasting through VMware or other virtual adapters in the common test setup.
- Decision: TCP server startup precedes discovery broadcasting.
  - A board should not discover an endpoint before TCP `9109` is being brought up.

## Protocol

```text
Destination: broadcast targets plus each peer in the selected x.y.z.0/24 subnet
Payload:     ARTPI_PC,1,9109\n
Fields:      magic,version,tcp_port
PC endpoint: recvfrom() source IPv4 + advertised TCP port
Cadence:     1 second
```

## Runtime Sequence
1. `server_config_init()` initializes the endpoint mutex.
2. `pc_discovery_init()` initializes the internal RT-Thread event.
3. Network management obtains a WiFi address.
4. `pc_discovery_start()` binds UDP `9108` and waits for broadcasts.
5. A valid packet commits the source IP and port to `server_config`, then publishes FOUND.
6. `tcp_client` connects to the committed endpoint and records its generation.
7. A later endpoint change increments generation; the TCP loop exits and reconnects.

## Risks / Trade-offs
- Windows Firewall may block outbound UDP broadcast or inbound TCP `9109`.
  - Mitigation: allow Python on Private networks and verify `netstat -ano | findstr :9109`.
- Some hotspot implementations may suppress all client broadcast while still permitting client unicast.
  - Mitigation: retain both broadcast forms and add a two-second `/24` unicast sweep fallback.
- Default-route selection can prefer a VPN or Ethernet interface.
  - Mitigation: disconnect competing default routes during testing or add an explicit interface override in a future change.
- Firmware compilation has not been verified locally because Keil MDK/armclang is unavailable.
  - Mitigation: build both `.uvprojx` projects before hardware testing.

## Migration Plan
1. Start the updated PC frontend.
2. Build and flash both updated ART-Pi2 projects once.
3. Verify initial discovery and TCP connection.
4. Restart the phone hotspot and reconnect the PC without rebooting the boards.
5. Confirm both boards discover the new address and reconnect automatically.

## Acceptance Record (2026-07-14)
- Hardware confirmed initial PC discovery, atomic endpoint publication, TCP connection, HELLO, JSON transfer, and PING/PONG closure.
- Hardware confirmed hotspot loss detection, AP retry, WiFi address recovery, discovery socket rebuild, new TCP session creation, and JSON/PONG recovery without rebooting the board.
- The tested phone hotspot filtered both limited and directed broadcast; the `/24` unicast sweep fallback restored automatic discovery.
- A later failed reconnect attempt was traced to the PC and board being on different hotspot subnets, not to discovery failure.
- The user accepted the capability without waiting for DHCP to naturally assign a different PC IPv4 address. The generation-change implementation remains covered by static review and the successful manual discovery path, with this un-reproduced transition retained as a residual risk.
