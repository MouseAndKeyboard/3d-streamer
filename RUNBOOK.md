# Runbook - cube-streamer

This runbook documents operational tasks for the single-droplet deployment.

## Service layout

- Releases live in `/opt/cube-streamer/releases/<release-id>`
- `/opt/cube-streamer/current` points to the active release
- Client assets are rsynced into `/opt/cube-streamer/current/client`
- `/var/www/cube-client` is a symlink to `/opt/cube-streamer/current/client`
- The systemd unit is `cube-server.service`
- The systemd unit should execute `/opt/cube-streamer/current/bin/cube_server`

## One-time setup

- Create the client symlink: `sudo ln -sfn /opt/cube-streamer/current/client /var/www/cube-client`

## Health checks

- HTTPS client: `https://stream.winnowscout.com/`
- WebSocket signaling: `wss://stream.winnowscout.com/ws/`
- Nginx health endpoint: `https://stream.winnowscout.com/healthz`

## Common commands

- Service status: `sudo systemctl status cube-server`
- Restart service: `sudo systemctl restart cube-server`
- Reload Nginx: `sudo systemctl reload nginx`
- Follow server logs: `sudo journalctl -u cube-server -f`
- Follow Nginx logs: `sudo tail -f /var/log/nginx/access.log /var/log/nginx/error.log`

## Rollback (artifact deploy)

1. List available releases:
   - `ls -1 /opt/cube-streamer/releases`
2. Point `current` at the last known-good release:
   - `sudo ln -sfn /opt/cube-streamer/releases/<release-id> /opt/cube-streamer/current`
3. Restart the server:
   - `sudo systemctl restart cube-server`
4. Re-run health checks.

## Rollback (container deploy)

If deploying via containers, switch to the previous image tag and restart the
service or `docker compose` stack.

## Escalation

- Primary contact: TBD
- Secondary contact: TBD

## Notes

- Ensure DNS points at the droplet before issuing TLS certificates.
- If WebSocket connections fail, confirm the `/ws/` proxy target and firewall
  rules for TCP 443 and UDP media ports.
