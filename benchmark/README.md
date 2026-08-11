# h2diagent benchmark

End-to-end **load** benchmark for the gateway. It reuses the `examples/`
deployment tooling (`1.deploy.sh` / `2.run.sh` / `4.trigger.sh` / `5.stop.sh`),
drives a fixed amount of Diameter traffic, and records latency, throughput and
gateway CPU/memory into a JSONL history.

Unlike the `diametercodec` micro-benchmark (pure CPU cost of encode/decode),
this exercises the whole path -- Diameter peer -> h2diagent -> h2agent -> back --
so it reflects the codec **and** the async transport (diametercomm) under real
traffic.

## What it measures

It is a **fixed-load** probe: the chosen `.example` pins the request count and
CPS. So the meaningful before/after comparison (e.g. across an optimization) is:

- **latency** of the Diameter round-trip (from the client gateway histogram
  `diameter_client_response_delay_seconds`): avg and p50/p90/p99, and
- **CPU / memory** of the h2diagent containers under that load (`docker stats`).

Throughput (answers/s) simply tracks the offered CPS unless the gateway
saturates; it is recorded for context, not as the headline figure.

## Prerequisites

- `docker`, `docker-compose`, `curl`, `python3`, `awk`.
- The `h2diagent` and `h2agent` images available locally (override with the
  `H2DIAGENT_IMAGE` / `H2AGENT_IMAGE` env vars used by `create-peer.sh`). To
  benchmark local changes, build the image first (`./build.sh --image`).

## Run

```bash
# default: gx-high-load (1000 CCR-I @ 100 CPS), human-readable summary
./benchmark/run.sh

# emit one JSON line to stdout instead of the summary; keep containers up
./benchmark/run.sh gx-high-load --jsonl --no-teardown
```

Options: `--jsonl` (machine-readable line to stdout instead of the summary),
`--timeout S` (completion wait, default 120), `--interval S` (sampling/poll
period, default 1), `--no-teardown` (leave containers running for inspection).

The script deploys, fires the trigger, waits until the client gateway has
received the expected number of answers (parsed from the trigger's
`sequenceEnd`, or a progress plateau otherwise), then prints the report.

Processing is done by `report.py` (Prometheus scraping, histogram percentiles,
`docker stats` aggregation); it runs on the host and **writes nothing to disk**
-- capture stdout yourself if you want to keep a record, e.g.:

```bash
./benchmark/run.sh gx-high-load --jsonl >> my-results.jsonl
```

## Output

Human summary (default) reports throughput, Diameter round-trip latency
(avg/p50/p90/p99) and per-container CPU/memory. With `--jsonl` the same data is
printed as a single JSON line. Raw latency/CPU numbers are only comparable **on
the same hardware**, hence the `hw_key` tag. Fields:

| field                | meaning                                                     |
|----------------------|-------------------------------------------------------------|
| `timestamp`          | UTC ISO-8601                                                |
| `git_commit` / `git_describe` / `git_dirty` | source revision                      |
| `hw_key` / `cpu_model` / `nproc` | hardware fingerprint                             |
| `example` / `cps`    | load profile                                                |
| `requests_sent` / `answers_received` | counter deltas over the run                 |
| `duration_s` / `throughput_rps` | run duration and answers per second              |
| `latency`            | `{avg_s, p50_s, p90_s, p99_s, count}` (percentiles are histogram bucket upper bounds, in seconds) |
| `resources`          | per h2diagent container `{cpu_avg_pct, cpu_peak_pct, mem_peak_mib, samples}` |

Example `--jsonl` line (formatted for readability; printed as a single line):

```json
{"timestamp":"2026-08-11T19:50:00Z","git_commit":"fa41837","git_dirty":false,
 "hw_key":"935158b2","cpu_model":"Intel(R) Xeon(R) Gold 6342 CPU @ 2.80GHz","nproc":8,
 "example":"gx-high-load","cps":"100","requests_sent":1000,"answers_received":1000,
 "duration_s":10.4,"throughput_rps":96.2,
 "latency":{"avg_s":0.0031,"p50_s":0.0025,"p90_s":0.005,"p99_s":0.01,"count":1000},
 "resources":{"pgw-load-h2diagent":{"cpu_avg_pct":18.3,"cpu_peak_pct":41.0,"mem_peak_mib":22.5,"samples":11}}}
```

## Notes for stable numbers

- Compare only runs of the same `example` on the same `hw_key` and image build.
- Keep the host otherwise idle; `docker stats` CPU% is relative to one core, so
  values above 100% mean multiple cores.
- To benchmark an optimization: run on `HEAD~1`'s image, then on the current
  image, and diff the two `--jsonl` outputs captured for the same `hw_key`.
- Offline sanity check of the analyzer: `python3 benchmark/report.py --selftest`.
