# Backend Streaming Server Design

This document describes the initial architecture for the backend streaming
server that renders a rotating cube and streams it to web clients in real time.
It is optimized for a single client and low operational complexity, with a
clear path to multi-client expansion.

## Goals
- Render a rotating 3D cube in a headless server process.
- Stream the rendered frames to a browser with low latency.
- Keep the system small, observable, and easy to debug.

## Non-goals (initial)
- Multi-region or HA.
- Complex scene graphs or user interaction.
- Multi-client scaling beyond a handful of viewers.

## Key decisions
- Language: C.
- Rendering: EGL + OpenGL ES (offscreen).
- Streaming: GStreamer with `appsrc` + `webrtcbin` (H.264 over WebRTC).
- Signaling: libwebsockets with a minimal JSON protocol.

## High-level architecture

```
Render Loop (EGL/GL) -> Raw RGBA frames -> appsrc -> GStreamer pipeline
    -> H.264 RTP -> webrtcbin -> WebRTC -> Browser

WebSocket Signaling (libwebsockets) <-> Browser JS (offer/answer/ICE)
```

## Components

1. **Renderer**
   - Owns EGL context and offscreen framebuffer.
   - Updates cube rotation each frame.
   - Reads pixels into a CPU buffer via `glReadPixels`.

2. **Frame Publisher**
   - Wraps raw frame bytes into `GstBuffer`.
   - Pushes buffers into `appsrc` with timestamps.
   - Backpressure-aware (drops or blocks when downstream is slow).

3. **GStreamer Pipeline**
   - Converts RGBA to I420.
   - Encodes H.264 with low-latency settings.
   - Packages RTP and hands off to `webrtcbin`.

4. **Signaling Server**
   - WebSocket endpoint for offer/answer/ICE.
   - Drives `webrtcbin` negotiation callbacks.
   - Manages client lifecycle and cleanup.

## Pipeline design

### Baseline pipeline (single client)

```
appsrc name=src is-live=true format=time do-timestamp=true
  caps=video/x-raw,format=RGBA,width=WIDTH,height=HEIGHT,framerate=FPS/1
  ! queue leaky=downstream max-size-buffers=2
  ! videoconvert
  ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=BITRATE key-int-max=FPS
  ! h264parse config-interval=-1
  ! rtph264pay pt=96 config-interval=-1
  ! application/x-rtp,media=video,encoding-name=H264,payload=96
  ! webrtcbin name=webrtc
```

Notes:
- `appsrc` is live and timestamped to avoid buffering.
- `queue` is leaky to keep latency bounded.
- `key-int-max=FPS` gives roughly 1-second GOP for faster recovery.

### Hardware acceleration (future)
- Swap `x264enc` with a hardware encoder (VAAPI/VideoToolbox/NVENC) when
  available, keeping the rest of the pipeline intact.

## Rendering details

- Use EGL surfaceless or pbuffer to avoid an on-screen window.
- Render at fixed resolution (e.g., 1280x720) and FPS (e.g., 30).
- Maintain a monotonic clock for timestamps and cube rotation.
- Use a single color/depth buffer and simple shader pair.

## Signaling protocol (minimal JSON)

Messages over WebSocket:
- Client -> Server: `{"type":"offer","sdp":"..."}`
- Server -> Client: `{"type":"answer","sdp":"..."}`
- Client -> Server: `{"type":"ice","candidate":"...","sdpMLineIndex":0}`
- Server -> Client: `{"type":"ice","candidate":"...","sdpMLineIndex":0}`

Flow:
1. Client connects and sends `offer`.
2. Server sets remote description, creates `answer`, sends it back.
3. ICE candidates exchanged until connection established.

## Threading model

- **Main thread**: initializes EGL/GL, starts GStreamer main loop.
- **Render thread**: renders frames and pushes buffers to `appsrc`.
- **Signaling thread**: runs libwebsockets service loop.
- Cross-thread communication via thread-safe queues:
  - Signaling thread posts messages to the main thread to call GStreamer APIs.
  - Render thread pushes into `appsrc` directly; GStreamer is thread-safe here.

## Lifecycle & error handling

- On first client connect, build pipeline and move to PLAYING.
- On disconnect, set pipeline to NULL and free resources.
- If `appsrc` returns `GST_FLOW_FLUSHING` or `EOS`, stop render loop.
- Log state changes and negotiation errors with enough detail for debugging.

## Configuration (env vars)

All config uses the `CS_` prefix:
- `CS_HOST` (default `0.0.0.0`)
- `CS_PORT` (default `8080`)
- `CS_WIDTH` / `CS_HEIGHT` (default `1280` / `720`)
- `CS_FPS` (default `30`)
- `CS_BITRATE_KBPS` (default `2500`)
- `CS_STUN_SERVER` (optional)
- `CS_TURN_SERVER`, `CS_TURN_USER`, `CS_TURN_PASS` (optional)

## Edge cases and mitigations

- **Slow encoder/network**: leaky queue prevents latency buildup; drop frames.
- **Client reconnects**: teardown and rebuild pipeline to keep state clean.
- **ICE failures**: expose error to logs; allow reconnect.
- **GL context loss**: reset EGL context and reinitialize resources.
- **Backpressure**: block or drop in render loop based on `appsrc` flow returns.

## Future expansion

- Multi-client: single shared encoder + tee to per-client `webrtcbin`, or
  per-client pipeline with shared raw frame queue.
- Metrics: expose FPS, encode time, and client count.
- Recording: branch encoder output to a file sink.

