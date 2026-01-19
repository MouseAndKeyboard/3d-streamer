# Observability - cube-streamer

Baseline monitoring keeps the single-node service debuggable and alertable.

## Logs

- Server logs live in journald under `cube-server.service`.
- Nginx logs live in `/var/log/nginx/`.

Recommended journald retention (edit `/etc/systemd/journald.conf`):

- `SystemMaxUse=1G`
- `MaxRetentionSec=30day`

After changes:

- `sudo systemctl restart systemd-journald`

## Metrics (optional)

Install node_exporter for host metrics:

1. Download the latest release from GitHub.
2. Create a `node_exporter` user.
3. Install the binary to `/usr/local/bin/node_exporter`.
4. Add a systemd unit to expose `:9100` on localhost.

If exposing externally, gate with firewall rules or a reverse proxy.

## Uptime checks

Create an external check that hits:

- `https://stream.winnowscout.com/healthz` (expect `200 OK`)

Alert on:

- TLS failures
- 5xx responses
- Timeouts over 10s

## Quick inspection

- Active connections: `sudo ss -tuna | rg ':443|:9000'`
- Disk usage: `df -h`
- Memory pressure: `free -h`
