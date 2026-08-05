# C++ HTTP/2 - DIAMETER Gateway Service

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/testillano/h2diagent/actions/workflows/ci.yml/badge.svg)](https://github.com/testillano/h2diagent/actions/workflows/ci.yml)
[![Documentation](https://codedocs.xyz/testillano/h2diagent.svg)](https://codedocs.xyz/testillano/h2diagent/index.html)

`h2diagent` is a lightweight **Diameter <-> HTTP/2 translation gateway** that enables [h2agent](https://github.com/testillano/h2agent) to mock and generate Diameter traffic without native Diameter support.

It translates Diameter messages to/from JSON over HTTP/2, allowing h2agent's full translation engine (sources, filters, targets, FSM, schemas) to drive Diameter test scenarios for interfaces like Gx, Rx, Sy, etc.

## Architecture

```
                    +----------------------------------+
                    |          h2diagent               |
                    |                                  |
  SUT <--Diameter-->|  Diameter server (:3868)         |
                    |  Diameter client (-> SUT)        |
                    |                                  |
                    |  HTTP/2 client  (-> h2agent)     |<-- inbound Diameter
                    |  HTTP/2 server  (:8080)          |<-- outbound Diameter
                    +----------------------------------+
                                    | HTTP/2
                    +----------------------------------+
                    |           h2agent                |
                    |  Traffic server (:8000) <- mock  |
                    |  Traffic client -> h2diagent     |
                    |  Admin API (:8074)               |
                    +----------------------------------+
```

## How it works

**Inbound flow** (SUT -> mock):
1. SUT sends Diameter request (e.g., CCR) to h2diagent
2. h2diagent decodes AVPs and maps them to JSON
3. Forwards as HTTP/2 POST to h2agent (e.g., `POST /diameter/gx/CCR`)
4. h2agent processes with provisions/transforms, responds with JSON
5. h2diagent maps JSON back to Diameter AVPs and sends answer to SUT

**Outbound flow** (mock -> SUT):
1. h2agent client sends HTTP/2 POST to h2diagent (e.g., `POST /diameter/rx/RAR`)
2. h2diagent maps JSON to Diameter AVPs and sends request to SUT
3. SUT responds with Diameter answer
4. h2diagent maps answer to JSON and returns HTTP/2 response to h2agent

## Dependencies

| Component | Repository |
|-----------|-----------|
| Diameter codec | [testillano/diametercodec](https://github.com/testillano/diametercodec) |
| Diameter communications | [testillano/diametercomm](https://github.com/testillano/diametercomm) |
| HTTP/2 communications | [testillano/http2comm](https://github.com/testillano/http2comm) |
| Metrics | [testillano/metrics](https://github.com/testillano/metrics) |
| Logger | [testillano/logger](https://github.com/testillano/logger) |

## Build with Docker

```bash
$ ./build.sh --image   # builds runtime image (ghcr.io/testillano/h2diagent:latest)
$ ./build.sh --builder # builds only deps stage
$ ./build.sh --ut      # builds unit test image
```

Or directly with Docker:

```bash
$ docker build --target runtime   -t h2diagent .
$ docker build --target deps      -t h2diagent_builder .
$ docker build --target unit-test -t h2diagent_ut .
```

### Building against a local (unpublished) diametercomm

By default the `deps` stage downloads the released `diametercomm`
(`ert_diametercomm_ver` in the `Dockerfile`). To iterate on **unpublished**
`diametercomm` changes (e.g. before pushing/tagging a new version), inject your
local working tree into the build:

```bash
# From the h2diagent repo root. Re-run whenever diametercomm changes.
$ rsync -a --delete \
    --exclude='.git' --exclude='build' --exclude='CMakeCache.txt' --exclude='CMakeFiles' \
    /path/to/testillano_diametercomm.master/ deps-local/diametercomm/

$ ./build.sh --image   # log shows ">>> Using LOCAL diametercomm source"
```

If `deps-local/diametercomm/CMakeLists.txt` is present the `deps` stage builds
that source instead of the released tarball; otherwise it falls back to
`ert_diametercomm_ver`, so canonical/CI builds are unaffected. `deps-local/` is
git-ignored. Once your `diametercomm` change is published, remove
`deps-local/diametercomm` and bump `ert_diametercomm_ver` to the new version.

## Unit tests

```bash
$ ./ut.sh
```

## Execution

```bash
$ docker run --rm -it -p 3868:3868 -p 8074:8074 -p 8080:8080 ghcr.io/testillano/h2diagent:latest \
    --origin-host "mock-pcrf.example.com" \
    --origin-realm "example.com" \
    --dictionary /config/dictionary.json \
    --h2agent-host h2agent \
    --h2agent-port 8000 \
    --verbose
```

## Command-line options

```
h2diagent - C++ HTTP/2 - DIAMETER Gateway Service (translation agent)

Diameter:
  --diameter-port <port>          Diameter listen port (default: 3868)
  --diameter-peer-host <host>     Remote Diameter peer host (for outbound)
  --diameter-peer-port <port>     Remote Diameter peer port (default: 3868)
  --origin-host <identity>        Origin-Host for CER
  --origin-realm <realm>          Origin-Realm for CER
  --product-name <name>           Product-Name for CER (default: h2diagent)
  --dictionary <path>             Diameter dictionary JSON file path
  --watchdog-interval <seconds>   DWR interval (default: 30)
  --diameter-server-transport <tcp|sctp>  Inbound server (listener) transport;
                                  sctp is single-homing (default: tcp)
  --diameter-client-transport <tcp|sctp>  Outbound client (to peer) transport;
                                  sctp is single-homing (default: tcp)

HTTP/2 (towards h2agent):
  --h2agent-host <host>           h2agent traffic server host (default: localhost)
  --h2agent-port <port>           h2agent traffic server port (default: 8000)

HTTP/2 (for outbound from h2agent):
  --http2-server-port <port>      HTTP/2 listen port for h2agent client (default: 8080)

General:
  --workers <n>                   Worker threads (default: nproc)
  --log-level <level>             Log level (default: Warning)
  --verbose                       Output log traces on console
  --admin-port <port>             Admin API port (default: 8074)
  --prometheus-port <port>        Prometheus scrape port (default: 9090)
  --disable-metrics               Disable prometheus metrics
  --version                       Program version
  --help                          This help
```

## Peers (quick Diameter endpoint setup)

The `create-peer.sh` tool generates self-contained Diameter peers without needing to understand the internal architecture. Each peer is a directory under `peers/` with everything needed to run.

### Create a peer

```bash
# Interactive:
./tools/create-peer.sh

# Non-interactive (default output: ./peers/):
./tools/create-peer.sh -n pcrf-mock -s "gx rx" -r server --server-port 3868 --admin-port 8274

# Custom output dir (e.g., for git-tracked examples):
./tools/create-peer.sh -n pgw-sim -s gx -r client --peer-host localhost --peer-port 3868 -o ./examples/peers
```

### Run peers

```bash
source examples/peers/pcrf-mock/run.bash    # starts Diameter server (PCRF)
source examples/peers/pgw-sim/run.bash      # starts Diameter client (PGW), connects to PCRF
```

### Stop peers

```bash
source examples/peers/pcrf-mock/stop.bash
source examples/peers/pgw-sim/stop.bash
```

### Peer directory structure

```
peers/<name>/
  config.env              # generated configuration variables
  docker-compose.yml      # opaque: h2diagent + h2agent sidecar
  stacks/                 # Diameter dictionaries (from tools/stacks/)
    gx.json
    rx.json
  programming/            # h2agent provisions (mock behavior)
    server-matching.json
    server-provision.json
  run.bash                # start the peer
  stop.bash               # stop the peer
```

### Multi-stack support

A single peer can handle multiple Diameter applications. Use `--dictionary` multiple times (or space-separated stacks in `create-peer.sh`):

```bash
./tools/create-peer.sh -n pcrf-mock -s "gx rx sy" -r server
```

All loaded Application-IDs are advertised in the CER/CEA and each message is decoded/encoded with the correct dictionary based on its Application-ID header.

### Available stacks

| Stack | Application-ID | Interface |
|-------|---------------|-----------|
| gx | 16777238 | Policy and Charging Control (Gx) |
| rx | 16777236 | Policy Control over Rx |
| sy | 16777302 | Spending Limit (Sy) |

Custom stacks can be added to `tools/stacks/` or directly into `peers/<name>/stacks/`.

## Metrics and Monitoring

h2diagent exposes Prometheus metrics on port `8085` (configurable via `--prometheus-port`).

Combined with h2agent's metrics (port `8080`), you get full visibility of both the Diameter and HTTP/2 sides.

### Quick Prometheus setup

Use h2agent's [`prometheus-only.sh`](https://github.com/testillano/h2agent/blob/master/tools/grafana/prometheus-only.sh) script to quickly spin up a Prometheus instance that scrapes both:

```bash
# From h2agent checkout:
./tools/grafana/prometheus-only.sh 8080 8085   # h2agent:8080 + h2diagent:8085
```

### Available metrics

Metrics are exposed in two groups: **Diameter** (from `diametercomm` library via `enableMetrics()`) and **HTTP/2** (instrumented at Gateway level).

The label '**source**' corresponds to the `--product-name` command-line parameter (default: `h2diagent`).

#### Diameter server (inbound)

```
Counters provided by diametercomm library:

   diameter_server_requests_received_counter [source] [command_code]
   diameter_server_answers_sent_counter [source] [command_code] [result_code]
   diameter_server_peer_connections_counter [source] [state: open/closed]

Gauges provided by diametercomm library:

   diameter_server_active_peers_gauge [source]
```

#### Diameter client (outbound)

```
Counters provided by diametercomm library:

   diameter_client_requests_sent_counter [source] [command_code]
   diameter_client_answers_received_counter [source] [command_code] [result_code]
   diameter_client_requests_timedout_counter [source] [command_code]
   diameter_client_requests_unsent_counter [source] [command_code]

Gauges provided by diametercomm library:

   diameter_client_peer_state_gauge [source] (1=open, 0=closed)
```

#### HTTP/2 server (outbound triggers from h2agent)

```
Counters provided by h2diagent:

   h2diagent_http2_server_requests_received_counter [source] [method]
   h2diagent_http2_server_responses_sent_counter [source] [method] [status_code]
```

#### HTTP/2 client (inbound translation towards h2agent)

```
Counters provided by h2diagent:

   h2diagent_http2_client_requests_sent_counter [source] [method]
   h2diagent_http2_client_responses_received_counter [source] [method] [status_code]
```

#### Examples

```bash
diameter_server_requests_received_counter{source="h2diagent",command_code="272"} 15
diameter_client_answers_received_counter{source="h2diagent",command_code="272",result_code="2001"} 15
h2diagent_http2_server_requests_received_counter{source="h2diagent",method="POST"} 15
h2diagent_http2_client_responses_received_counter{source="h2diagent",method="POST",status_code="200"} 15
diameter_server_active_peers_gauge{source="h2diagent"} 1
diameter_client_peer_state_gauge{source="h2diagent"} 1
```

### Grafana dashboard

A Grafana dashboard JSON is provided at `tools/grafana/grafana/provisioning/dashboards/h2diagent.json`.
Import it into your Grafana instance and configure the Prometheus datasource. See `tools/grafana/README.md` for details.

## Related projects

- [h2agent](https://github.com/testillano/h2agent) - HTTP/2 mock service (the brain)
- [diametercodec](https://github.com/testillano/diametercodec) - Diameter codec library
- [diametercomm](https://github.com/testillano/diametercomm) - Diameter communications library

## Contributing

Check the project [contributing guidelines](./CONTRIBUTING.md).

## License

[MIT](LICENSE)
