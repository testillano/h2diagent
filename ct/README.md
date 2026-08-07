# h2diagent component test (CT)

Automated component test for `h2diagent`, mirroring the
[h2agent CT](https://github.com/testillano/h2agent/tree/master/ct): a Helm
deployment on Kubernetes (minikube) plus a `pytest` suite that provisions mock
behaviour, fires Diameter traffic, and asserts on the recorded data.

## Topology

Two Diameter peers are deployed, each a pod with **two containers**:

- `h2diagent` : the Diameter <-> HTTP/2 translation gateway
- `h2agent`   : the HTTP/2 mock "brain" (sidecar, reached over `localhost`)

```
                    test pod (pytest)
                     |            |
        admin API    |            |   admin API
                     v            v
   +-------------------+      +-------------------+
   |   client peer     |      |   server peer     |
   |  h2agent (mock)   |      |  h2agent (mock)   |
   |  h2diagent(client)|      |  h2diagent(server)|
   +---------|---------+      +---------^---------+
             |   Diameter CCR (:3868)   |
             +--------------------------+
```

- **server** (`role=server`): `h2diagent` listens for inbound Diameter on
  `:3868`; its `h2agent` mocks the answer (e.g. a Gx CCA).
- **client** (`role=client`): `h2diagent` dials `server:3868`; its `h2agent`
  triggers the outbound request (e.g. a Gx CCR).

Both peers use identical ports (they are separate pods/Services) and are
addressed by Service DNS name (`server`, `client`).

## Charts

- `helm/h2diagent` : reusable chart for one peer (h2diagent + h2agent sidecar),
  parametrized by `role`, `originHost`, `stacks`, `diameter.*`, etc.
- `helm/ct-h2diagent` : wrapper that depends on `h2diagent` twice (aliases
  `server` and `client`) and adds the pytest runner pod.

Diameter dictionaries are bundled into the `h2diagent` chart's `dictionaries/`
directory (git-ignored) by `ct/test.sh`, sourced from `tools/stacks/`.

## Build the images

```bash
# From the repo root: builds the runtime (h2diagent) AND the CT image:
./build.sh --image     # -> ghcr.io/testillano/h2diagent:latest
                       #    ghcr.io/testillano/ct-h2diagent:latest

# Rebuild just the CT (pytest) image:
./build.sh --ct
```

On minikube, images built with the cluster's Docker daemon are immediately
available to pods (no push required).

## Run the CT

```bash
./ct/test.sh                 # deploy + test + hints (default)
./ct/test.sh deploy          # only deploy the chart
./ct/test.sh test            # only run pytest (chart already deployed)
./ct/test.sh hints           # only print inspection hints

# Run a single test file / selector:
./ct/test.sh test gx_functional/gxf_ccri_test.py
./ct/test.sh test gx_functional/gxf_ccri_test.py -k ccri
```

### Prepend variables

| Variable          | Default  | Meaning                                   |
|-------------------|----------|-------------------------------------------|
| `TAG`             | `latest` | `h2diagent` image tag                     |
| `CT_TAG`          | `latest` | `ct-h2diagent` (pytest) image tag         |
| `H2AGENT_TAG`     | `latest` | `h2agent` sidecar image tag               |
| `STACKS_TO_BUNDLE`| `gx`     | space-separated Diameter stacks to bundle |
| `XTRA_HELM_SETS`  | -        | extra `--set` args for `helm install`     |
| `SKIP_HELM_DEPS`  | -        | non-empty skips `helm dep update`         |

```bash
TAG=dev CT_TAG=dev ./ct/test.sh
STACKS_TO_BUNDLE="gx rx" ./ct/test.sh deploy
```

## Test cases

- `gx_functional/gxf_ccri_test.py` : a single Gx CCR-I / CCA-I through the full
  pipeline. Asserts the server peer recorded the inbound CCR and the client peer
  received a CCA with `Result-Code=2001` and the `Session-Id` echoed back.

## Inspecting a running deployment

```bash
NS=ns-ct-h2diagent
server_pod=$(kubectl get pod -n $NS -l app.kubernetes.io/name=server -o name | head -1)
client_pod=$(kubectl get pod -n $NS -l app.kubernetes.io/name=client -o name | head -1)
kubectl port-forward -n $NS $server_pod 8074:8074 &   # server h2agent admin
kubectl port-forward -n $NS $client_pod 8075:8074 &   # client h2agent admin

curl -s --http2-prior-knowledge http://localhost:8074/admin/v1/server-data | jq .
curl -s --http2-prior-knowledge http://localhost:8075/admin/v1/client-data | jq .
```

Data recorded by the *last* test remains available (the suite cleans provisions
and data only *before* each test), so the commands above show real content.

Inside the `h2agent` sidecars, the h2agent helper functions are pre-loaded
(sourced into `~/.bashrc`), bound to that peer's ports:

```bash
kubectl exec -it -n $NS $server_pod -c h2agent -- bash
# then, e.g.:  server_data | jq .   /   schema ...   /   server_provision ...
```

## Notes

- SCTP transport is out of scope for the CT (needs the host `sctp` kernel
  module / host networking); use the `examples/gx-functional-sctp` demo for
  that. The CT uses TCP.
- `h2diagent` has no admin/health endpoint yet, so its container readiness is a
  TCP check on the HTTP/2 server port; the `h2agent` sidecar uses its own
  `/admin/v1/health`.
- **Startup ordering**: Kubernetes does not guarantee container/pod start order.
  `h2diagent` now reconnects automatically (the Diameter client uses
  diametercomm's exponential backoff, and the HTTP/2 client towards `h2agent`
  recreates its session with backoff), so peers recover regardless of start
  order and survive `h2agent`/peer restarts. As a startup-latency optimization,
  the chart still wraps the `h2diagent` container to wait (bash `/dev/tcp`) for
  the local `h2agent` sidecar and, for a client peer, the remote Diameter peer's
  port before starting, avoiding an initial backoff cycle and error logs. This
  wait is now optional for correctness.
- **Helpers on exec**: both containers pre-load helper functions into
  `~/.bashrc` (sourced on interactive `kubectl exec`):
  - the `h2agent` sidecars load the h2agent helpers (`server_data`,
    `client_data`, `server_provision`, ...), bundled by `ct/test.sh` from the
    h2agent tools (`H2AGENT_HELPERS_BASH`, default sibling
    `../testillano_h2agent.master`); if not found, only the port variables load.
  - the `h2diagent` containers load this repo's `tools/helpers.bash` (Diameter
    metric shortcuts: `metrics`, `traffic_summary`), bound to the gateway's
    Prometheus port.
  Disable via `h2agent.utilsMountPath=""` / `utilsMountPath=""`.
