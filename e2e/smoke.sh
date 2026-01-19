#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: ./smoke.sh --base-url URL [options]

Options:
  --base-url URL       Base HTTP/HTTPS URL (or CS_BASE_URL)
  --ws-url URL         WebSocket URL (or CS_WS_URL)
  --health-path PATH   Health path (or CS_HEALTH_PATH, default /healthz)
  --timeout SECONDS    Curl timeout (or CS_TIMEOUT, default 10)
  --insecure           Allow insecure TLS (or CS_INSECURE=1)
  --help               Show this help
USAGE
}

BASE_URL="${CS_BASE_URL:-}"
WS_URL="${CS_WS_URL:-}"
HEALTH_PATH="${CS_HEALTH_PATH:-/healthz}"
TIMEOUT="${CS_TIMEOUT:-10}"
INSECURE="${CS_INSECURE:-0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base-url)
      BASE_URL="$2"
      shift 2
      ;;
    --ws-url)
      WS_URL="$2"
      shift 2
      ;;
    --health-path)
      HEALTH_PATH="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    --insecure)
      INSECURE=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown flag: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$BASE_URL" ]]; then
  echo "Missing --base-url (or CS_BASE_URL)." >&2
  usage >&2
  exit 1
fi

BASE_URL="${BASE_URL%/}"
if [[ "$HEALTH_PATH" != /* ]]; then
  HEALTH_PATH="/$HEALTH_PATH"
fi

if [[ -z "$WS_URL" ]]; then
  if [[ "$BASE_URL" == http://* ]]; then
    WS_URL="ws://${BASE_URL#http://}/ws"
  elif [[ "$BASE_URL" == https://* ]]; then
    WS_URL="wss://${BASE_URL#https://}/ws"
  else
    echo "Unable to derive WS URL from base URL: $BASE_URL" >&2
    exit 1
  fi
fi

CURL_FLAGS=("--fail" "--silent" "--show-error" "--max-time" "$TIMEOUT")
if [[ "$INSECURE" == "1" ]]; then
  CURL_FLAGS+=("-k")
fi

HEALTH_URL="${BASE_URL}${HEALTH_PATH}"

echo "[smoke] GET $HEALTH_URL"
if ! curl "${CURL_FLAGS[@]}" "$HEALTH_URL" > /dev/null; then
  echo "[smoke] Health check failed at $HEALTH_URL" >&2
  exit 1
fi

ws_accept() {
  local key="$1"
  printf '%s' "${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11" |
    openssl sha1 -binary |
    openssl base64
}

WS_KEY="$(openssl rand -base64 16 | tr -d '\n')"
EXPECTED_ACCEPT="$(ws_accept "$WS_KEY" | tr -d '\n')"

printf '[smoke] WS upgrade %s\n' "$WS_URL"
RESPONSE="$(curl "${CURL_FLAGS[@]}" -i --http1.1 \
  -H "Connection: Upgrade" \
  -H "Upgrade: websocket" \
  -H "Sec-WebSocket-Version: 13" \
  -H "Sec-WebSocket-Key: ${WS_KEY}" \
  "$WS_URL" || true)"

if ! printf '%s' "$RESPONSE" | tr -d '\r' | head -n 1 | grep -q "101"; then
  echo "[smoke] WebSocket upgrade failed" >&2
  echo "$RESPONSE" >&2
  exit 1
fi

if ! printf '%s' "$RESPONSE" | tr -d '\r' | grep -iq "^Sec-WebSocket-Accept: ${EXPECTED_ACCEPT}$"; then
  echo "[smoke] WebSocket accept mismatch" >&2
  echo "$RESPONSE" >&2
  exit 1
fi

echo "[smoke] OK"
