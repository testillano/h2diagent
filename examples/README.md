# Examples

Diameter gateway scenarios. Each `.example` file is a self-contained test configuration.

## Commands

```bash
./1.deploy.sh <example>    # create deployment from .example file
./2.run.sh                 # start the current deployment
./3.status.sh              # show container status
./4.trigger.sh             # fire traffic
./5.stop.sh                # teardown
```

Only `deploy.sh` takes an argument. The rest operate on the generated `./deployment`.

## Available examples

### gx-functional

Single CCR-I/CCA-I through the full Diameter translation pipeline.

```bash
./1.deploy.sh gx-functional
./2.run.sh
./4.trigger.sh

# Verify:
curl -sf --http2-prior-knowledge http://localhost:8174/admin/v1/client-data | python3 -m json.tool
curl -sf --http2-prior-knowledge http://localhost:8274/admin/v1/server-data | python3 -m json.tool

./5.stop.sh
```

### gx-high-load

1000 CCR-I at 100 CPS. Data storage disabled for performance.

```bash
./1.deploy.sh gx-high-load
./2.run.sh
./4.trigger.sh

# Monitor:
curl -sf http://localhost:11274/metrics | grep diameter   # client (pgw-load)
curl -sf http://localhost:11374/metrics | grep diameter   # server (pcrf-load)

./5.stop.sh
```

### gx-functional-sctp

Same as `gx-functional` but the Diameter transport is **SCTP single-homing** on
both roles (server listener and client dialer). Functionally identical to the
TCP flow; only the transport differs.

```bash
./1.deploy.sh gx-functional-sctp
./2.run.sh
./4.trigger.sh

# Verify the SCTP association is established (host networking):
ss --sctp -a | grep -E ':3868'

# Verify the translation pipeline (same as gx-functional):
curl -sf --http2-prior-knowledge http://localhost:8174/admin/v1/client-data | python3 -m json.tool
curl -sf --http2-prior-knowledge http://localhost:8274/admin/v1/server-data | python3 -m json.tool

./5.stop.sh
```

Requires SCTP support in the host kernel (module `sctp`) since the containers
use `network_mode: host`; the h2diagent image already ships `libsctp1`.

### gx-functional-tls

Same as `gx-functional` but the Diameter hop is **secured with TLS over TCP** on
both roles (the server presents a certificate; the client verifies it against
the CA). The HTTP/2 hop towards h2agent stays clear text.

`1.deploy.sh` generates a throwaway CA plus server/client certificates
(`tools/ssl/create-certs.sh`) into `./deployment/ssl` and mounts them read-only
at `/ssl` in every h2diagent container.

```bash
./1.deploy.sh gx-functional-tls
./2.run.sh
./4.trigger.sh

# The gateway logs show the secured mode, e.g.:
#   Diameter server listening on port 3868 [TLS]
#   Diameter client connecting to localhost:3868 [TLS]
docker logs pcrf-mock-h2diagent 2>&1 | grep -E "listening|\[TLS\]"

# Optional: confirm the TLS handshake / server certificate on the wire:
openssl s_client -connect localhost:3868 -CAfile ./deployment/ssl/ca.crt </dev/null 2>/dev/null \
  | grep -E "subject=|Verify return code"

# Verify the translation pipeline (same as gx-functional):
curl -sf --http2-prior-knowledge http://localhost:8174/admin/v1/client-data | python3 -m json.tool
curl -sf --http2-prior-knowledge http://localhost:8274/admin/v1/server-data | python3 -m json.tool

./5.stop.sh
```

TLS applies to the **TCP** transport only; there is no DTLS/SCTP support (see the
project README "gap" note). Enabling `SSL=1` in a `.example` triggers the cert
generation and the `/ssl` mount; the TLS flags go in `EXTRA_H2DIAGENT_ARGS`.


## Ports reference

Ports are derived from admin port (configured in each .example):

| Port | Formula | Description |
|------|---------|-------------|
| Admin (h2agent) | `--admin-port` | Provisioning and inspection |
| Traffic (h2agent) | admin + 1000 | Internal (h2diagent <-> h2agent) |
| Prometheus (h2agent) | admin + 2000 | h2agent metrics |
| Prometheus (h2diagent) | admin + 3000 | Diameter/HTTP/2 gateway metrics |
| HTTP/2 server (h2diagent) | admin + 4000 | Outbound triggers (client role only) |

For the provided examples:

| Example | Peer | Admin | h2diagent metrics |
|---------|------|-------|-------------------|
| gx-functional | pcrf-mock | 8274 | 11274 |
| gx-functional | pgw-sim | 8174 | 11174 |
| gx-high-load | pcrf-load | 8374 | 11374 |
| gx-high-load | pgw-load | 8274 | 11274 |

## Creating new examples

Create a `<name>.example` file:

```bash
# Description of the test (shown by deploy.sh --help)

PEERS=(
    "server-name server gx --server-port 3868 --admin-port 8274 --origin-host srv.example.com --origin-realm example.com"
    "client-name client gx --peer-host localhost --peer-port 3868 --admin-port 8174 --origin-host cli.example.com --origin-realm example.com"
)

EXTRA_H2AGENT_ARGS=""         # e.g., "--discard-data"
PEERING_WAIT=4                # seconds to wait for CER/CEA
TRIGGER="http://localhost:8174/admin/v1/client-provision/send-gx-ccr"
```

Then: `./1.deploy.sh my-test && ./2.run.sh && ./4.trigger.sh`
