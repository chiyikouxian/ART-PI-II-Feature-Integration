## 1. Specification and Design
- [x] 1.1 Define the UDP discovery protocol and port ownership.
- [x] 1.2 Define atomic endpoint generation and TCP reconnect behavior.
- [x] 1.3 Define discovery event lifecycle and stop/start semantics.
- [x] 1.4 Document that ROCK endpoints are outside this capability.

## 2. PC Frontend
- [x] 2.1 Add the UDP discovery broadcaster using Python standard-library sockets.
- [x] 2.2 Select and bind the default-route IPv4 interface and rebuild the socket after an IP change.
- [x] 2.3 Start TCP `9109` before starting discovery broadcasts.
- [x] 2.4 Add rate-limited startup, IP-change, and error logging.
- [x] 2.5 Send both limited and `/24` directed broadcasts for Windows phone-hotspot compatibility.
- [x] 2.6 Add a two-second `/24` unicast sweep fallback for hotspots that filter all broadcast frames.

## 3. ART-Pi2 Firmware
- [x] 3.1 Add strict UDP discovery parsing to both firmware projects.
- [x] 3.2 Use the `recvfrom()` source address and advertised port as the PC endpoint.
- [x] 3.3 Add thread-safe endpoint snapshots and generation updates.
- [x] 3.4 Make both TCP clients reconnect when generation changes.
- [x] 3.5 Add WiFi down/up socket recovery and discovery statistics.
- [x] 3.6 Add persistent event initialization, reset, stop notification, and stop/start cleanup.
- [x] 3.7 Add `pc_discovery.c` to both Keil projects.

## 4. Documentation and Static Validation
- [x] 4.1 Synchronize the root and per-board README files.
- [x] 4.2 Pass Python AST syntax validation.
- [x] 4.3 Pass `git diff --check` apart from platform line-ending warnings.
- [x] 4.4 Run `openspec validate add-pc-endpoint-auto-discovery --strict --no-interactive`.

## 5. Build and Hardware Validation
- [x] 5.1 Accept the left-hand build based on the symmetric implementation review and the user's stage-acceptance decision; a separate local armclang rerun is unavailable on this PC.
- [x] 5.2 Verify the right-hand build through the flashed firmware used for the captured hardware logs.
- [x] 5.3 Verify TCP `9109`, automatic UDP discovery delivery, endpoint publication, HELLO, JSON frames, and PONG responses on hardware.
- [x] 5.4 Verify initial automatic discovery and connection on the active right-hand hardware; accept left-hand symmetry for this stage.
- [x] 5.5 Restart the phone hotspot and verify WiFi loss, repeated AP retry, IP acquisition, UDP socket rebuild, and TCP reconnection without rebooting the board.
- [x] 5.6 Accept the different-PC-IP DHCP transition without waiting for nondeterministic reassignment; manual unicast verified endpoint discovery and connection, and the generation-change path passed static review.
- [x] 5.7 Verify right-hand JSON and PONG recovery; accept ROCK-path isolation by code review and defer unrelated ROCK reachability errors.

## 6. Acceptance Record
- [x] 6.1 User accepted this capability stage on 2026-07-14.
- [x] 6.2 Record that the hotspot filters client broadcast, requiring the `/24` unicast sweep fallback.
- [x] 6.3 Record that a test with PC `192.168.245.217` and board `192.168.137.186` was invalid because the devices were connected to different hotspot subnets.
- [x] 6.4 Preserve the residual risk that a naturally reassigned PC IPv4 address was not reproduced end-to-end during this session.
