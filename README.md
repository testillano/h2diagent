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

## Diameter <-> JSON mapping

This section describes how each Diameter AVP **data format** is represented in
the JSON that h2diagent exchanges with h2agent:

- **client-provision** requests (h2agent -> h2diagent -> Diameter peer), and
- **server-provision** responses (Diameter peer -> h2diagent -> h2agent).

### Model: representation is driven by the AVP format in the dictionary

The JSON is **flat**: each AVP is a key (its dictionary name) mapping to a value.
There is **no per-value type/encoding flag**. The representation is **type-driven**:
the engine looks up the AVP's **format** in the loaded dictionary (`--dictionary`)
and encodes/decodes the JSON value according to the Diameter norm for that format.

Rules of thumb:
- **Numeric** formats -> JSON number, unquoted.
- **Textual** formats -> JSON string (human text).
- **OctetString** (opaque) -> JSON string containing **hex** (even number of
  hex digits).
- **Grouped** -> JSON object (repeated AVPs become a JSON array).

The AVP name is the JSON key. Vendor-specific AVPs are resolved by the
dictionary (the Vendor-Id / V-bit is taken from the dictionary entry); you only
write the AVP name, never the Vendor-Id.

### The `_header` reserved key (message header)

Besides AVPs, the JSON carries one **reserved key, `_header`**, that represents
the **Diameter message header** (version, flags, command-code, application-id,
request/answer, and the transaction identifiers). It is **not** an AVP: any key
named `_header` is treated as the header and is never encoded as an AVP.

```json
"_header": {
  "version": 1,
  "flags": 128,
  "command-code": 272,
  "request": true,
  "application-id": 16777238,
  "hop-by-hop-id": 0,
  "end-to-end-id": 0
}
```

| Field            | Type    | Meaning |
|------------------|---------|---------|
| `version`        | number  | Diameter version (always `1`). |
| `flags`          | number  | Command flags **bitmask**: `0x80` R (request), `0x40` P (proxiable), `0x20` E (error), `0x10` T (potentially re-transmitted). |
| `command-code`   | number  | Command code, e.g. `272` (CC), `265` (AA), `258` (RA), `275` (ST). |
| `request`        | boolean | `true` = request (sets the R-bit), `false` = answer. Overrides the R-bit in `flags`. |
| `application-id` | number  | Diameter Application-Id, e.g. `16777238` (Gx). |
| `hop-by-hop-id`  | number  | Transaction correlation id; `0` lets the client assign it. |
| `end-to-end-id`  | number  | End-to-end id; `0` lets the client assign it. |

When h2diagent **decodes** an inbound Diameter message to JSON (before POSTing
to h2agent), it always emits `_header` alongside the AVPs, so provisions can
read/match/transform on it. When h2diagent **encodes** JSON back to Diameter, it
reads `_header` if present (missing fields fall back to defaults).

#### The header is auto-managed; `_header` in a provision is an optional override

In normal operation you do **not** need to write `_header`: h2diagent fills it in
for you. Provide it only when you want to override the header.

> **Important - `_header` is used as-is (no merge).** If a provision includes
> `_header`, h2diagent uses it **verbatim** and does **not** merge it with the
> values it would otherwise compute. Any field you omit falls back to its default
> (`0` for the numeric ids). So a *partial* `_header` (e.g. only `flags`) will
> reset `command-code`, `application-id`, `hop-by-hop-id` and `end-to-end-id` to
> `0` -- which, for an answer, breaks correlation at the peer. If you override,
> provide the **complete** header.

- **Server-provision responses (inbound answer, peer -> h2agent -> peer).**
  The HTTP/2 response is correlated with the request at the stream level, and
  h2diagent already holds the original `command-code`, `application-id`,
  `hop-by-hop-id` and `end-to-end-id`. So whenever your response body has **no**
  `_header`, it **reconstructs the answer header from that request context**
  (with `request:false`, `flags:0`). This is the normal, recommended case: you
  do not mirror anything back (the HTTP/2 response headers are ignored).

  Because the answer's `hop-by-hop-id`/`end-to-end-id` are **dynamic** (per
  transaction) and a static provision does not know them, the practical way to
  override a header field (e.g. set the Error bit) is to **echo the request's
  `_header`** -- which is present in the inbound request JSON -- via an h2agent
  transform, and then adjust the field you want. Hand-writing a static `_header`
  in the response is only safe if you supply the correct dynamic ids too.

- **Client-provision requests (outbound, h2agent -> h2diagent -> peer).**
  When the request body has no `_header`, h2diagent **derives it from the request
  URI** `/diameter/<interface>/<command>` (mapping the interface to the
  Application-Id and the command abbreviation to the command-code), sets
  `request:true` / `flags:0x80`, and leaves `hop-by-hop-id`/`end-to-end-id` at
  `0` for the client to assign. Overriding here is practical: provide a
  **complete** `_header` to force a command-code/application-id the URI cannot
  express, or extra flags (`hop-by-hop-id`/`end-to-end-id` can stay `0`).

  ```json
  {
    "Session-Id": "client.example.com;sess;1",
    "Origin-Host": "client.example.com",
    "_header": {
      "version": 1, "flags": 128, "request": true,
      "command-code": 272, "application-id": 16777238,
      "hop-by-hop-id": 0, "end-to-end-id": 0
    }
  }
  ```

#### Related: `x-diameter-*` request headers (inbound POST to h2agent)

On the inbound flow, h2diagent also sets informational HTTP headers on the POST
to h2agent: `x-diameter-command-code`, `x-diameter-application-id`,
`x-diameter-hop-by-hop`, `x-diameter-end-to-end`, `x-diameter-flags`. They mirror
`_header` for convenience (matching, transforms, logging) and are **optional to
use**. They are not required on the response: the gateway does not read the
HTTP/2 response headers, because it rebuilds the answer header from the
correlated request as described above.

### Per-format table (the 16 base/derived formats)

| Diameter format   | JSON type | Example (JSON)                         | On-the-wire meaning |
|-------------------|-----------|----------------------------------------|---------------------|
| Integer32         | number    | `"CC-Request-Number": 0`               | signed 32-bit |
| Integer64         | number    | `"Accounting-Sub-Session-Id": 12345`   | signed 64-bit |
| Unsigned32        | number    | `"Auth-Application-Id": 16777238`      | unsigned 32-bit |
| Unsigned64        | number    | `"CC-Total-Octets": 1099511627776`     | unsigned 64-bit |
| Float32           | number    | `"Some-Float32": 1.5`                  | IEEE 754 single |
| Float64           | number    | `"Some-Float64": 1.5`                  | IEEE 754 double |
| Enumerated        | number    | `"CC-Request-Type": 1`                 | signed 32-bit (enum value; no alias in flat JSON) |
| OctetString       | string (hex) | `"Framed-IP-Address": "c0a80001"`   | raw bytes; `"af01"` -> 0xAF 0x01 |
| UTF8String        | string    | `"Subscription-Id-Data": "1234567890"` | UTF-8 text |
| DiameterIdentity  | string    | `"Origin-Host": "pcef.example.com"`    | text |
| DiameterURI       | string    | `"Redirect-Server-Address": "aaa://host:3868"` | text |
| IPFilterRule      | string    | `"IPFilterRule-Avp": "permit in ip from any to any"` | text |
| QoSFilterRule     | string    | `"QoSFilterRule-Avp": "..."`           | text |
| Time              | number    | `"Event-Timestamp": 1700000000`        | UNIX epoch seconds (engine converts to/from NTP) |
| Address           | string    | `"Some-Address": "192.168.0.2"`        | IPv4/IPv6 human form; engine encodes family+bytes on the wire |
| Grouped           | object    | see below                              | nested AVPs |

#### OctetString (hex)

OctetString carries opaque bytes, so it is written as a **hex string** (even
length). Examples:
```json
"Framed-IP-Address": "c0a80001",          // 4 raw bytes = 192.168.0.1
"3GPP-User-Location-Info": "8064f0000064" // packed binary, verbatim bytes
```
`"af01"` decodes to the two bytes 0xAF 0x01. The hex must be valid and even
length; invalid hex is an error (do not rely on silent parsing).

IMPORTANT for IP-bearing OctetStrings: many 3GPP AVPs (e.g. `Framed-IP-Address`,
AVP code 8) are declared **OctetString** and carry the **4 raw bytes** of the
IPv4 address per the norm. Therefore they must be written as hex of those 4
bytes (192.168.0.2 -> `"c0a80002"`), NOT as the dotted-decimal string
`"192.168.0.2"` and NOT as the ASCII of the dotted string. Writing a
dotted-decimal string into an OctetString produces wrong bytes and the peer will
reject the message (e.g. Result-Code 5004 DIAMETER_INVALID_AVP_VALUE).

#### Grouped

Repeated inner AVPs are expressed as a JSON array under the AVP name:
```json
"Subscription-Id": [
  { "Subscription-Id-Type": 1, "Subscription-Id-Data": "1234567890" }
],
"Supported-Features": [
  { "Vendor-Id": 10415, "Feature-List-ID": 1, "Feature-List": 11 }
]
```
On decode, a grouped AVP that appears multiple times is rendered as an array of
objects.

#### Time

Represented as **UNIX epoch seconds** (a plain number). The engine converts
to/from the Diameter NTP timestamp (adds/subtracts the NTP epoch offset).

### Caveats / notes (engine)

- **Address format**: handled per the Diameter norm. In JSON it is the human
  form (dotted IPv4 / IPv6 literal); on the wire the engine builds
  `address-family (2 bytes) + address bytes` (IPv4 = family 1 + 4 bytes,
  IPv6 = family 2 + 16 bytes) via inet_pton/inet_ntop.
  Note: `Framed-IP-Address` (AVP 8) is NOT `Address` format - it is declared
  `OctetString` (4 raw bytes), so it uses hex, not the dotted form.
- **Enumerated**: represented as the integer value in JSON; enumerated
  **aliases** (symbolic names) are not part of the JSON, but they ARE shown in
  the debug traces (see below).
- **OctetString hex**: must be strict, even-length hex.

### Debug traces (per-AVP format mapping)

When h2diagent runs with `--log-level Debug`, the codec emits a trace for every
AVP as it crosses between HTTP/2 (JSON) and Diameter (bytes), showing the
format-driven mapping. Examples:

```
Diameter encode AVP 'Framed-IP-Address' (OctetString): 0xc0a80001 -> 0xc0a80001
Diameter encode AVP 'CC-Request-Type' (Enumerated): 1 (INITIAL_REQUEST) -> 0x00000001
Diameter decode AVP 'Result-Code' (Unsigned32): 0x00001391 -> 5004
Diameter decode AVP 'Origin-Host' (DiameterIdentity): 0x... -> "pcrf.example.com"
```

For `Enumerated` AVPs the trace appends the dictionary alias/literal in
parentheses next to the numeric value.

### JSON mapping summary

Write each AVP value in the canonical representation for its dictionary format:
numbers for integer/unsigned/float/enum, human text for the text formats, UNIX
epoch for Time, hex for OctetString, and nested objects/arrays for Grouped. The
engine encodes to the normative Diameter wire bytes according to that format.

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

### Helper functions (`tools/helpers.bash`)

Since h2diagent has no administrative REST interface, `tools/helpers.bash`
provides shortcut functions over the Prometheus metrics endpoint (h2diagent
only; the h2agent sidecar has its own helpers). Source it natively, or use it
inside the container (the helm chart mounts it and sources it into `~/.bashrc`
for `kubectl exec`):

```bash
source tools/helpers.bash   # native (targets localhost:8085 by default)

metrics [port]           # raw Prometheus metrics scraped from h2diagent
traffic_summary [port]   # Diameter answers by result-code, pass(2001)/fail
```

Each function takes an optional `[port]` (overriding `METRICS_PORT`) and `-h`
for a one-line description. Override targets via `METRICS_PORT`, `SERVER_ADDR`,
`SCHEME`, `CURL`.

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
