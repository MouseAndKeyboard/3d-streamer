# End-to-End Streaming Validation Design

## Goal
Validate the full pipeline from signaling to live video playback so we can trust
"pixels on screen" for the cube-streamer stack.

## Scope
- Signaling reachability and WebSocket upgrade
- WebRTC connection establishment
- Video track reception and frame progression

## Non-goals
- Rendering quality checks (lighting, geometry correctness)
- Performance tuning beyond basic latency and frame cadence
- Multi-client scale or load testing (future work)

## Assumptions
- Signaling runs on the same host as the HTTP endpoint
- Default signaling path is `/ws`
- The server acts as the WebRTC offerer (client answers)

## Signaling contract (minimal JSON)
The validation client expects a simple JSON protocol:

- `{"type":"hello","role":"viewer","id":"e2e"}` sent on WS open
- `{"type":"offer","sdp":"..."}` sent by server
- `{"type":"answer","sdp":"..."}` sent by client
- `{"type":"ice","candidate":{...}}` sent both ways
- Optional: `{"type":"bye"}` to close

This keeps the handshake generic and easy to implement in C (libwebsockets)
while remaining compatible with browser WebRTC APIs.

## Validation layers

### 1) Smoke checks (fast)
- HTTP GET to `/healthz` (fallback to `/` if missing)
- WebSocket upgrade on `/ws` (verify 101 + Sec-WebSocket-Accept)

### 2) WebRTC probe (full E2E)
- Use a headless browser to open a dedicated probe page
- Connect to signaling over WS
- Accept the offer, send an answer, exchange ICE
- Assert:
  - `RTCPeerConnection.connectionState` reaches `connected`
  - A video track is received
  - Video frames advance over time (>= N frames in T seconds)

## Edge cases and risks
- Autoplay restrictions: use autoplay-friendly flags in headless Chromium
- NAT traversal: allow STUN/TURN configuration via CLI/env vars
- Flaky timing: set timeouts and retry thresholds; emit stats on failure
- TLS: allow `--insecure` for self-signed staging endpoints

## Outputs and pass/fail criteria
- PASS when connection is established and frames advance
- FAIL when WS upgrade fails, no offer arrives, ICE never connects, or frames
  do not advance within the timeout

## Implementation plan
- `e2e/smoke.sh`: HTTP + WebSocket upgrade checks
- `e2e/rtcp_probe.html`: self-contained WebRTC probe client
- `e2e/run-webrtc.js`: Playwright harness to drive the probe page headlessly
- `e2e/README.md`: how to run locally or against staging
- `e2e/package.json`: Playwright dependency only (kept isolated in `e2e/`)

## Future extensions
- Capture WebRTC stats (jitter, RTT, fps) into JSON for regression tracking
- Add multi-client soak test to validate server fan-out
- Integrate into CI once a headless environment is available
