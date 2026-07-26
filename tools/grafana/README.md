# Grafana Metrics for h2diagent

## Prometheus

Use h2agent's [`prometheus-only.sh`](https://github.com/testillano/h2agent/blob/master/tools/grafana/prometheus-only.sh)
to scrape h2diagent metrics:

```bash
prometheus-only.sh 11174 11274   # example: gx-high-load h2diagent ports
```

Prometheus ports are derived from admin port: `admin + 3000`. For the examples:

| Example | Peer | Admin | Prometheus (h2diagent) |
|---------|------|-------|------------------------|
| gx | pcrf-mock | 8274 | 11274 |
| gx | pgw-sim | 8174 | 11174 |
| gx-high-load | pcrf-load | 8374 | 11374 |
| gx-high-load | pgw-load | 8274 | 11274 |

## Dashboard

Import `h2diagent-dashboard.json` into Grafana.

Panels:
- **Overview**: Active server peers, client peer state, connection rate
- **Diameter Server (Inbound)**: Requests received / answers sent by command code
- **Diameter Client (Outbound)**: Requests sent / answers received, timeouts
- **Errors**: Unsent requests, cumulative errors
- **Gateway Latency**: Throughput, pending requests, timeout rate

Variables: `$datasource`, `$source`, `$rate_interval`
