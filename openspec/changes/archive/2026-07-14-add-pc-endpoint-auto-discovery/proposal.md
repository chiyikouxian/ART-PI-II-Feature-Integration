# Change: Add PC Endpoint Auto Discovery

## Why
The ART-Pi2 endpoints currently depend on a configured PC IPv4 address. When a phone hotspot is restarted, DHCP may assign the PC a different address, forcing a firmware configuration change and reflash before the gloves can reconnect.

## What Changes
- Add a PC-side UDP discovery service that advertises the frontend TCP port through limited broadcast, hotspot-directed broadcast, and a low-rate `/24` unicast sweep fallback.
- Add left-hand and right-hand UDP discovery listeners that use the validated packet source address as the PC IPv4 endpoint.
- Add an atomic `IP + port + generation` endpoint snapshot in `server_config` so a connected TCP client can detect endpoint changes and reconnect cleanly.
- Define discovery lifecycle, WiFi loss recovery, strict packet validation, statistics, and stop/start behavior.
- Keep the ROCK uplink endpoints and ports independent from PC discovery.
- Document the protocol, startup order, Windows firewall requirement, diagnostics, and hotspot-change validation procedure.

## Impact
- Affected specs: `pc-endpoint-auto-discovery` (new capability)
- Affected code: `leading_end/app/services/discovery_service.py`, `leading_end/run.py`, left/right `pc_discovery.c/.h`, `server_config.c/.h`, `tcp_client.c`, `main.c`, and both Keil project files
- Network ports: UDP `9108` for discovery and TCP `9109` for glove JSON/control traffic
- Unaffected paths: ROCK CSV/model/control sessions on ports `9101` and `9102`
