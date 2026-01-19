# Observability - cube-streamer

Baseline monitoring keeps the single-node service debuggable and alertable.

## Logs

- Server logs live in journald under `cube-server.service`.
- Nginx logs live in `/var/log/nginx/`.

Journald retention (drop-in example):

- `/etc/systemd/journald.conf.d/99-retention.conf`
- `SystemMaxUse=500M`
- `MaxRetentionSec=14day`
- `Compress=yes`

After changes:

- `sudo systemctl restart systemd-journald`

## Metrics (optional)

Install `prometheus-node-exporter` and pin it to localhost:

- Service: `prometheus-node-exporter.service`
- Override: `/etc/systemd/system/prometheus-node-exporter.service.d/override.conf`
- Listen address: `127.0.0.1:9100`

If exposing externally, gate with firewall rules or a reverse proxy.

## Uptime checks

Optional local uptime checks (systemd timer):

- Script: `/usr/local/bin/cube-uptime-check.sh`
- Timer: `cube-uptime-check.timer` (every 5 min)

Create an external check that hits:

- `https://stream.winnowscout.com/healthz` (expect `200 OK`)

Alert on:

- TLS failures
- 5xx responses
- Timeouts over 10s

## Quick inspection

- Active connections: `sudo ss -tuna | grep -E ':443|:9000'`
- Disk usage: `df -h`
- Memory pressure: `free -h`
