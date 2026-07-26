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

## Related projects

- [h2agent](https://github.com/testillano/h2agent) - HTTP/2 mock service (the brain)
- [diametercodec](https://github.com/testillano/diametercodec) - Diameter codec library
- [diametercomm](https://github.com/testillano/diametercomm) - Diameter communications library

## Contributing

Contributions are welcome. Please open an issue first to discuss what you would like to change.

## License

[MIT](LICENSE)
