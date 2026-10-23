# Operations runbook

## Monitoring and alerting

Use `/healthz` for a local or Nginx-proxied readiness probe. `prometheus.yml` and `prometheus-alerts.yml` provide a blackbox-exporter-based probe and alerts for availability and latency. Route Prometheus alerts to the organization’s Alertmanager, pager, or incident system.

The service logs to systemd-journald with the identifier `fakaapache`. Centralize those logs with the host’s journald forwarder or your organization’s log collector. Configure journald retention and disk limits centrally; `fakaapache.logrotate` is provided for deployments that redirect service logs to files.

## Certificate rotation

Terminate public TLS in Nginx. Use Certbot or the organization’s certificate manager, validate the renewed configuration with `nginx -t`, and run `certificate-renewal.sh` as the renewal deploy hook. Verify the public endpoint and certificate expiry after every rotation.

## Backups and rollback

Run `backup.sh` after each successful deployment and before configuration changes. Store archives outside the application host and periodically perform a restore test. To roll back, select a verified archive, run `rollback.sh`, then verify `/healthz`, the public Nginx endpoint, and application routes. Keep the previous release available until validation completes.

## Security review

Complete `SECURITY.md`, the smoke tests, CodeQL, dependency review, and an authorized independent penetration test before exposing the service publicly. Review Nginx route separation, proxy destinations, authentication, file permissions, TLS policy, and configuration secrets on every release.
