# Validation Record

Date: 2026-07-14

## Verified on Hardware
- PC TCP listener active on `0.0.0.0:9109`.
- ART-Pi2 received discovery from `192.168.137.217` and updated generation `0 -> 1`.
- TCP connected and emitted a new HELLO session.
- PC registered the right-hand device and received sequential JSON frames.
- PING/PONG application heartbeat remained active.
- Turning off the hotspot caused `READY -> LOST`, discovery socket closure, and TCP teardown.
- Re-enabling the same hotspot caused AP retries, IP reacquisition, discovery socket recreation, a new TCP session, and resumed JSON/PONG traffic.
- A manual UDP unicast packet proved that source-address discovery works when hotspot broadcast is filtered.
- The PC `/24` unicast sweep includes the observed board address and restores automatic discovery on the tested hotspot.

## Accepted Without Full Reproduction
- DHCP did not naturally assign a different PC IPv4 address while both devices remained on the same hotspot.
- The user accepted the generation-driven different-IP transition based on implementation review and the verified discovery/update/reconnect components.
- Left-hand behavior was accepted from the symmetric implementation for this stage; the captured detailed hardware log was from the right-hand endpoint.
- ROCK reachability errors were explicitly outside this PC-connection test; code-path isolation was reviewed instead.

## Invalid Test Excluded from Results
A test where the PC used `192.168.245.217` while the board used `192.168.137.186` was excluded because the devices were attached to different hotspot subnets and could not communicate directly.
