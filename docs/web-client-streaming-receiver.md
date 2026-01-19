# Web Client Streaming Receiver Design

## Goals
- Receive and render the live cube stream in a browser with low latency.
- Keep signaling minimal and compatible with the server's libwebsockets JSON.
- Support local dev (`ws://localhost:8080`) and production (`wss://...`).

## Non-goals
- Bidirectional control/input or data channels (for now).
- Multi-stream compositing, recording, or client-side transcoding.
- Complex UI/UX beyond connect, status, and playback.

## Assumptions
- Server uses GStreamer `webrtcbin` to send H.264 over WebRTC.
- Signaling is WebSocket-based, JSON messages, single stream per peer.
- The client is a passive receiver (server is the offerer).

## Architecture
Components:
- **UI**: connect/disconnect button, status text, `<video>` element.
- **SignalingClient**: WebSocket wrapper for JSON send/receive.
- **Receiver**: manages `RTCPeerConnection`, media tracks, ICE.
- **Config**: signaling URL, ICE servers, debug flags.

High-level flow:
1. User clicks Connect (satisfies autoplay requirements).
2. Open WebSocket, send `hello`/`ready`.
3. Receive SDP offer, create answer, exchange ICE candidates.
4. Attach remote stream to `<video>` and display.

## Signaling Protocol (proposed)
Minimal JSON messages (tolerant to extra fields):

```json
{ "type": "hello", "role": "viewer", "client_id": "optional" }
```

```json
{ "type": "offer", "sdp": "..." }
```

```json
{ "type": "answer", "sdp": "..." }
```

```json
{
  "type": "ice",
  "candidate": "candidate:...",
  "sdpMid": "0",
  "sdpMLineIndex": 0
}
```

```json
{ "type": "bye", "reason": "server_shutdown" }
```

Client behavior:
- On `offer`: `pc.setRemoteDescription`, `pc.createAnswer`, `pc.setLocalDescription`, send `answer`.
- On `ice`: call `addIceCandidate` (queue candidates until remote description is set).

If the server expects the client to create the offer instead, flip roles:
- Client creates offer after `ready`, sends `offer`, then processes `answer`.

## Connection Lifecycle
State machine:

```
IDLE -> CONNECTING -> NEGOTIATING -> STREAMING -> RETRYING -> CONNECTING
```

Details:
- **CONNECTING**: open WebSocket, create `RTCPeerConnection`, add a recvonly
  transceiver for video.
- **NEGOTIATING**: wait for offer, respond with answer; exchange ICE.
- **STREAMING**: on `track`, attach `event.streams[0]` to video element.
- **RETRYING**: backoff and re-init if WS or ICE fails.

## Error Handling and Reconnect
- WebSocket close/error: show status, schedule reconnect with exponential
  backoff (1s, 2s, 4s, max 15s).
- ICE state `failed` or `disconnected`: attempt `pc.restartIce()` once, then
  full teardown/reconnect if no recovery.
- Unexpected message types: log and ignore.
- Oversized messages: drop and warn (protects against malformed input).

## UI/UX
- Simple layout: status pill, connect/disconnect button, video element.
- `video` should use `autoplay`, `playsInline`, and `muted` to avoid autoplay
  blocking; user gesture initiates connection for safety.
- Optional debug panel: bitrate, fps, packet loss from `pc.getStats()`.

## Configuration
- **Signaling URL**: `?ws=ws://localhost:8080` query param or config constant.
- **ICE servers**: optional STUN/TURN array; default to no TURN.
- **Auto-connect**: allowed in dev only.

## Security Considerations
- Use `wss` in production to avoid mixed-content blocks.
- Strict JSON parsing; ignore unexpected fields.
- Avoid exposing detailed stack traces in UI by default.

## Testing Plan (for later)
- Unit: signaling message parser and state transitions.
- Manual: connect to local server, verify playback, verify reconnect behavior.
- Fault injection: drop WS, force ICE failure, ensure recovery works.

## Open Questions
- Final offerer/answerer roles: assume server offers; confirm with server code.
- Multi-client: is a `client_id` required by signaling? If so, persist it.
- Audio: server currently video-only; ensure client expects no audio tracks.
