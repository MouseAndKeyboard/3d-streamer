# E2E Streaming Validation

This folder contains scripts and a probe client to validate the end-to-end
streaming pipeline (signaling + WebRTC + video frames).

## Prerequisites
- Node.js 18+
- npm
- A running signaling server (`/ws`) and WebRTC offerer

## Install

```bash
cd e2e
npm install
npm run install-browsers
```

## Smoke checks (HTTP + WS)

```bash
./smoke.sh --base-url https://stream.example.com
```

Options:
- `--base-url` (or `CS_BASE_URL`)
- `--ws-url` (or `CS_WS_URL`)
- `--health-path` (or `CS_HEALTH_PATH`, default `/healthz`)
- `--timeout` (or `CS_TIMEOUT`, seconds)
- `--insecure` (or `CS_INSECURE=1`)

## Full WebRTC probe (headless)

```bash
node run-webrtc.js --ws-url wss://stream.example.com/ws
```

Options:
- `--ws-url` (or `CS_WS_URL`)
- `--base-url` (or `CS_BASE_URL`, used to derive WS URL)
- `--stun` (or `CS_STUN_SERVER`)
- `--timeout-ms` (or `CS_TIMEOUT_MS`)
- `--min-frames` (or `CS_MIN_FRAMES`)
- `--headed` (run with a visible browser)
- `--insecure` (ignore TLS errors)
- `--verbose` (emit console logs)

The probe passes when a WebRTC connection is established and video frames
advance within the timeout.
