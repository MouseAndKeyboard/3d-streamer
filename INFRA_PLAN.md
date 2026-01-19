# Infra + Deployment Plan (DigitalOcean + GitHub Actions)

This plan targets the cube-streamer repo as described in `README` and assumes a
single render/stream server with a static web client. It is intentionally small
and incrementally upgradeable.

## Formula mapping (bd formulas)

These are interpreted as deployment recipes with increasing rigor:

- **shiny**: single droplet, minimal hardening, manual deploys, best for dev or
  a quick staging box.
- **shiny-secure**: single droplet with hardening, locked-down SSH, firewall,
  automated patching, CI/CD with protected environments, basic observability.
- **shiny-enterprise**: multi-node, HA, separate services (DB/logs/metrics),
  audit trails, SSO, compliance controls.

**Recommendation**
- Use **shiny-secure** for production from day one.
- Use **shiny** for an early staging box if speed matters more than hardening.
- **shiny-enterprise** is not needed until multi-region, HA, or compliance
  requirements appear.

## Architecture snapshot

- **One DigitalOcean droplet** (Ubuntu LTS) running:
  - `cube_server` (custom C code, built around GStreamer) as a systemd service.
  - Static client assets served by a reverse proxy (Caddy or Nginx).
  - Signaling over HTTPS + WebSocket; media via WebRTC (UDP).
- **DNS + TLS** for a single domain (e.g., `stream.example.com`).
  - Optional: automate DNS records via Cloudflare API using GitHub Actions.

## DigitalOcean provisioning plan

### Droplet sizing
- Start: 2 vCPU / 4 GB RAM (upgrade if GPU or software encoding needs more).
- Region: closest to primary testers to reduce latency.

### OS baseline
- Ubuntu 22.04 LTS or 24.04 LTS.
- Enable automatic security updates.
- Enable NTP (systemd-timesyncd).

### Users + SSH
- Create `deploy` user with sudo.
- Disable root SSH, disable password auth.
- Enforce SSH key auth only.
- Optional: restrict SSH to known IPs via DO firewall.

### Firewall (DO + UFW)
- Allow TCP 22 from trusted IPs only.
- Allow TCP 80/443 for web + signaling.
- Allow UDP range for WebRTC (e.g., 10000-20000) or document the exact range
  chosen by the ICE configuration.
- Deny everything else by default.

### TLS + reverse proxy
- Prefer **Caddy** for auto HTTPS and simple config.
- Proxy `/ws` (or chosen path) to the signaling server.
- Serve static client from `/var/www/cube-client` or from the container.
- Point `/var/www/cube-client` at `/opt/cube-streamer/current/client`.
- Reference Nginx vhost: `ops/nginx/streamer.conf` (includes `/healthz`).

### Service management
- `cube_server` systemd unit with:
  - Restart on failure.
  - Logs to journald.
  - Env file at `/etc/cube-server/env`.
  - ExecStart at `/opt/cube-streamer/current/bin/cube_server`.

### Optional hardening
- `fail2ban` for SSH.
- `ufw` rules mirrored to DO firewall.
- Turn on swap if memory is tight.

## CI/CD plan (GitHub Actions)

### Workflows
1. **CI** (`.github/workflows/ci.yml`)
   - Trigger: PRs and pushes to `main`.
   - Steps: build server (`cmake`), build client (`npm`), run unit tests (when
     added), archive artifacts.
2. **Deploy staging** (`deploy-staging.yml`)
   - Trigger: push to `main`.
   - Builds artifacts, deploys to staging droplet.
3. **Deploy prod** (`deploy-prod.yml`)
   - Trigger: manual `workflow_dispatch` or tag (e.g., `v*`).
   - Required reviewers via GitHub Environments.

Workflows upload a release bundle to `/opt/cube-streamer/releases/<sha>` and
update `/opt/cube-streamer/current`.

### Deployment method (simple + reliable)
Option A (preferred): **containerized deploy**
- Build a Docker image for server + static client.
- Push to GHCR.
- Droplet pulls and restarts via systemd or `docker compose`.

Option B: **artifact deploy**
- `rsync` server binary + client `dist/` to droplet.
- Atomically swap `current` symlink.

### Secrets management
- Use GitHub Environments (`staging`, `production`) with environment secrets:
  - `SSH_HOST`, `SSH_USER`, `SSH_KEY`
  - `CS_STUN_SERVER`, `CS_TURN_*` (if needed)
  - `DOMAIN`, `TLS_EMAIL`
  - `CLOUDFLARE_API_KEY` (Cloudflare API token/key for DNS automation)
- Also store the GitHub repository secret `DIGITAL_OCEAN_KEY` (a DigitalOcean
  API key) for provisioning/monitoring environments from CI/CD. DigitalOcean
  API docs: https://docs.digitalocean.com/reference/api/digitalocean/
- Keep runtime secrets in `/etc/cube-server/env`, owned by root.

### Rollback
- Keep last N releases on droplet.
- Roll back by switching `current` symlink or pulling previous image tag.
See `RUNBOOK.md` for step-by-step rollback and operational commands.

## Observability plan (small-service friendly)
See `OBSERVABILITY.md` for baseline logging, uptime checks, and optional metrics.

### Logs
- Journaled logs for server and proxy.
- Optional: ship to Grafana Cloud Loki via `promtail`.

### Metrics
- `node_exporter` for host metrics.
- Add minimal app metrics (fps, encode time, connected clients) later using a
  Prometheus endpoint or StatsD.

### Traces
- Not required initially. Add OpenTelemetry only if debugging latency issues.

### Alerting
- Uptime check on HTTPS endpoint.
- Alerts for CPU/RAM/disk pressure, service restarts, and 5xx rate.

## Environment strategy

- **dev**: local build, self-signed or no TLS, local config.
- **staging**: single DO droplet, basic TLS, auto deploy on main.
- **production**: hardened droplet, restricted SSH, manual deploy gate.

Configuration:
- Use env vars with `CS_` prefix.
- Keep non-secret config in repo (example `.env.sample`).
- Store secrets only in GitHub and `/etc/cube-server/env`.

## Step-by-step checklist

### Initial bootstrap
1. Create droplet and attach reserved IP.
2. Configure DNS for `stream.example.com`.
3. Create `deploy` user + SSH keys.
4. Apply firewall rules (DO + UFW).
5. Install packages: build tools, GStreamer, libwebsockets, Caddy/Nginx.
6. Install `cube_server` systemd unit.
7. Configure TLS and reverse proxy.
8. Deploy first build manually to validate runtime.
9. Add GitHub Actions workflows + environment secrets.
10. Run a full CI deploy to staging.

### Ongoing releases
1. Merge to `main`.
2. CI builds + tests.
3. Deploy staging automatically.
4. Promote to prod via manual workflow or tag.
5. Verify health endpoint + client playback.
6. Rollback if needed.

## Open questions / assumptions

- Do we want container-based deploys or bare-metal artifacts?
- What domain will be used for TLS?
- Do we need a TURN server, or is STUN-only acceptable?
- Expected concurrent viewer count and target latency?
- Any cost limits for DO sizing or managed services?
- Should staging be optional (skip for very early stage)?
