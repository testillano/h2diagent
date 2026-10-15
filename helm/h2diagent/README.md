# h2diagent Helm chart

Deploys a single Diameter peer whose core is **h2diagent**, a pure
**Diameter <-> HTTP/2 translator**.

## Pure translator by default (no companion h2agent)

By default (`h2agent.enabled=false`) the chart deploys **one container**: just the
gateway. h2diagent has **no embedded programming/provisioning engine** -- it only
translates. That engine lives in a separate **h2agent**, and whether an h2agent
needs to accompany the gateway depends on the Diameter role the gateway plays.

Historically the gateway and an h2agent looked inseparable, because the clean
design would have been to put the programming engine *inside* h2diagent; reusing
an h2agent as that engine was the pragmatic shortcut. In practice that coupling
only holds for one of two very different roles:

### The two roles of the companion h2agent (same name, opposite jobs)

* **MOCK brain -- justifies a sidecar.**
  When the gateway is a Diameter **SERVER** (PCRF-like), inbound Diameter must be
  *answered*, so the gateway needs a brain to decide the response. That brain is an
  h2agent acting as a **mock**, reached over `localhost`. Co-locating it as a
  **sidecar in this pod is natural** and is the only case that justifies the
  companion container. Enable it with `h2agent.enabled=true`.

* **DIRECTOR -- belongs in an EXTERNAL pod.**
  A real **director** is not "the gateway's brain": it is a **coordinator of hybrid
  HTTP/2 + Diameter flows**. It drives a chain whose steps are partly plain HTTP/2
  that *never touch the gateway* and partly Diameter that do. Its scope exceeds the
  gateway, so its natural home is a **separate pod**, not a sidecar of this chart.
  For a pure Diameter **CLIENT** / translator there is no inbound to answer and
  therefore **no companion h2agent is needed** here at all.

Because of this, the default is **no companion** (pure translator). Only the
server/PCRF mock-brain case turns the sidecar on.

## Values

| Key | Default | Meaning |
|-----|---------|---------|
| `h2agent.enabled` | `false` | Run the h2agent mock-brain as a sidecar in this pod (server/PCRF case). When `false`, the gateway is a single-container pure translator. |
| `h2agent.host` | `""` | External brain to dial (a Service DNS of an h2agent in another pod) when `enabled=false`. Empty = run with no brain (valid for a pure client/translator). |
| `h2agent.port` | `8000` | Traffic port of the external brain (used with `h2agent.host`). |
| `role` | `server` | Diameter role: `server` (listen), `client` (dial), `both`. |
| `diameter.serverPort` | `3868` | Inbound Diameter listener (server/both; `0` disables). |
| `diameter.peerHost` / `peerPort` | `""` / `3868` | Remote peer dialed in client/both mode. |
| `http2ServerPort` | `8081` | HTTP/2 server where an (external) director posts outbound triggers. Exposed in the Service as `http2-trigger`. |
| `prometheusPort` | `8085` | Gateway metrics port (Service `gw-metrics`). |

The effective brain the gateway dials is:

* `enabled=true`  -> `localhost:h2agent.trafficPort` (sidecar);
* `enabled=false` + `host` set -> `host:port` (external pod);
* `enabled=false` + `host` empty -> no `--h2agent-host` passed (no brain).

## Service

The Service always exposes the gateway's `http2-trigger` (and, for a server,
`diameter`, plus `gw-metrics` when metrics are enabled). The h2agent ports
(`http2-admin`, `http2-traffic`, `http-metrics`) are exposed **only** when
`h2agent.enabled=true`.

## Component test note

The component test (`ct-h2diagent`) runs each peer as a **monopod**
(`h2agent.enabled=true`) for convenience, with the h2agent playing the **mock**
role on the server peer and the **director** role on the client peer. That is a CT
convenience over `localhost`, not the chart default; see `ct-h2diagent/values.yaml`.
