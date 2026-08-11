#!/usr/bin/env python3
# =============================================================================
# report.py - Analyze an h2diagent load run and append a JSONL history record.
# =============================================================================
# Consumes two Prometheus text snapshots (before/after the load) scraped from
# the CLIENT gateway, plus a 'docker stats' sample file, and produces (all on
# STDOUT, nothing written to disk):
#   - a human-readable summary (default), or
#   - one machine-readable JSON line (--jsonl), tagged with git + hardware
#     context; redirect it yourself if you want to keep a record.
#
# Stdlib only (no third-party deps). See --selftest for an offline sanity check.
# =============================================================================

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
import time

# Client-gateway metric names (see diametercomm/DiameterClient.cpp).
M_ANSWERS = "diameter_client_answers_received_counter"
M_REQUESTS = "diameter_client_requests_sent_counter"
M_DELAY = "diameter_client_response_delay_seconds"  # histogram: _bucket/_sum/_count


def parse_counter(text, name):
    """Sum a counter family across all its label sets."""
    total = 0.0
    # Match 'name' or 'name{labels}' followed by a value; ignore _bucket/_sum/_count of others.
    pat = re.compile(r"^" + re.escape(name) + r"(?:\{[^}]*\})?\s+([0-9eE.+-]+)\s*$")
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = pat.match(line)
        if m:
            total += float(m.group(1))
    return total


def parse_histogram(text, base):
    """Aggregate a histogram family: returns (buckets{le->cum_count}, sum, count)."""
    buckets = {}
    hsum = 0.0
    hcount = 0.0
    b_pat = re.compile(r"^" + re.escape(base) + r"_bucket\{([^}]*)\}\s+([0-9eE.+-]+)\s*$")
    s_pat = re.compile(r"^" + re.escape(base) + r"_sum(?:\{[^}]*\})?\s+([0-9eE.+-]+)\s*$")
    c_pat = re.compile(r"^" + re.escape(base) + r"_count(?:\{[^}]*\})?\s+([0-9eE.+-]+)\s*$")
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        mb = b_pat.match(line)
        if mb:
            labels, val = mb.group(1), float(mb.group(2))
            le_m = re.search(r'le="([^"]+)"', labels)
            if le_m:
                le = le_m.group(1)
                buckets[le] = buckets.get(le, 0.0) + val
            continue
        ms = s_pat.match(line)
        if ms:
            hsum += float(ms.group(1))
            continue
        mc = c_pat.match(line)
        if mc:
            hcount += float(mc.group(1))
    return buckets, hsum, hcount


def _le_key(le):
    return float("inf") if le in ("+Inf", "Inf") else float(le)


def histogram_delta_percentiles(before, after, percentiles):
    """Percentiles (in seconds) from the delta of two cumulative bucket sets.

    Buckets are cumulative (Prometheus semantics). Returns dict pctl->seconds
    (bucket upper bound; +Inf reported as None) plus avg and count."""
    b_buckets, b_sum, b_count = before
    a_buckets, a_sum, a_count = after

    les = sorted(set(list(a_buckets.keys()) + list(b_buckets.keys())), key=_le_key)
    delta = [(le, a_buckets.get(le, 0.0) - b_buckets.get(le, 0.0)) for le in les]
    total = a_count - b_count
    dsum = a_sum - b_sum

    out = {"count": total, "avg_s": (dsum / total) if total > 0 else None, "pctl": {}}
    for p in percentiles:
        if total <= 0:
            out["pctl"][p] = None
            continue
        target = (p / 100.0) * total
        chosen = None
        for le, cum in delta:
            if cum >= target:
                chosen = le
                break
        out["pctl"][p] = None if (chosen in (None, "+Inf", "Inf")) else float(chosen)
    return out


def parse_docker_stats(path):
    """Parse the sampler file: lines '<name> <cpu%> <memMiB>'. Returns per-name
    dict {cpu_avg, cpu_peak, mem_peak_mib, samples}."""
    agg = {}
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        return agg
    for line in lines:
        parts = line.split()
        if len(parts) < 3:
            continue
        name, cpu, mem = parts[0], parts[1], parts[2]
        try:
            cpu_v = float(cpu.rstrip("%"))
            mem_v = float(mem)
        except ValueError:
            continue
        d = agg.setdefault(name, {"cpu": [], "mem": []})
        d["cpu"].append(cpu_v)
        d["mem"].append(mem_v)
    result = {}
    for name, d in agg.items():
        if not d["cpu"]:
            continue
        result[name] = {
            "cpu_avg_pct": round(sum(d["cpu"]) / len(d["cpu"]), 1),
            "cpu_peak_pct": round(max(d["cpu"]), 1),
            "mem_peak_mib": round(max(d["mem"]), 1),
            "samples": len(d["cpu"]),
        }
    return result


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def git_first_line(args):
    try:
        out = subprocess.run(["git"] + args, capture_output=True, text=True, timeout=5)
        return out.stdout.strip().splitlines()[0] if out.stdout.strip() else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def build_record(a):
    with open(a.before) as f:
        before_txt = f.read()
    with open(a.after) as f:
        after_txt = f.read()

    answers = parse_counter(after_txt, M_ANSWERS) - parse_counter(before_txt, M_ANSWERS)
    requests = parse_counter(after_txt, M_REQUESTS) - parse_counter(before_txt, M_REQUESTS)
    hist = histogram_delta_percentiles(
        parse_histogram(before_txt, M_DELAY), parse_histogram(after_txt, M_DELAY), [50, 90, 99]
    )
    stats = parse_docker_stats(a.stats) if a.stats else {}

    throughput = (answers / a.duration) if a.duration and a.duration > 0 else None

    import os

    model = cpu_model()
    nproc = os.cpu_count() or 0
    hw_key = hashlib.sha1(f"{model}x{nproc}".encode()).hexdigest()[:8]

    record = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "git_commit": git_first_line(["rev-parse", "--short", "HEAD"]) or "unknown",
        "git_describe": git_first_line(["describe", "--tags", "--always", "--dirty"]),
        "git_dirty": bool(git_first_line(["status", "--porcelain"])),
        "hw_key": hw_key,
        "cpu_model": model,
        "nproc": nproc,
        "example": a.example,
        "cps": a.cps,
        "requests_sent": int(requests),
        "answers_received": int(answers),
        "duration_s": round(a.duration, 3) if a.duration else None,
        "throughput_rps": round(throughput, 1) if throughput is not None else None,
        "latency": {
            "avg_s": round(hist["avg_s"], 6) if hist["avg_s"] is not None else None,
            "p50_s": hist["pctl"][50],
            "p90_s": hist["pctl"][90],
            "p99_s": hist["pctl"][99],
            "count": int(hist["count"]),
        },
        "resources": stats,
    }
    return record


def print_summary(r):
    print("\nh2diagent load benchmark")
    print(f"  example       : {r['example']}  (cps={r['cps']})")
    print(f"  commit        : {r['git_commit']}{' (dirty)' if r['git_dirty'] else ''}")
    print(f"  requests sent : {r['requests_sent']}")
    print(f"  answers recv  : {r['answers_received']}")
    print(f"  duration      : {r['duration_s']} s")
    print(f"  throughput    : {r['throughput_rps']} answers/s")
    lat = r["latency"]

    def ms(v):
        return f"{v * 1000:.2f} ms" if v is not None else "n/a"

    print(f"  latency avg   : {ms(lat['avg_s'])}")
    print(f"  latency p50   : <= {ms(lat['p50_s'])}")
    print(f"  latency p90   : <= {ms(lat['p90_s'])}")
    print(f"  latency p99   : <= {ms(lat['p99_s'])}")
    if r["resources"]:
        print("  resources (per container):")
        for name, s in r["resources"].items():
            print(
                f"    {name}: cpu avg {s['cpu_avg_pct']}% peak {s['cpu_peak_pct']}% | "
                f"mem peak {s['mem_peak_mib']} MiB ({s['samples']} samples)"
            )


def selftest():
    before = """# HELP x
diameter_client_answers_received_counter{source="pgw",command_code="272"} 0
diameter_client_requests_sent_counter{source="pgw",command_code="272"} 0
diameter_client_response_delay_seconds_bucket{le="0.001"} 0
diameter_client_response_delay_seconds_bucket{le="0.005"} 0
diameter_client_response_delay_seconds_bucket{le="0.01"} 0
diameter_client_response_delay_seconds_bucket{le="0.05"} 0
diameter_client_response_delay_seconds_bucket{le="+Inf"} 0
diameter_client_response_delay_seconds_sum 0
diameter_client_response_delay_seconds_count 0
"""
    after = """diameter_client_answers_received_counter{source="pgw",command_code="272"} 1000
diameter_client_requests_sent_counter{source="pgw",command_code="272"} 1000
diameter_client_response_delay_seconds_bucket{le="0.001"} 100
diameter_client_response_delay_seconds_bucket{le="0.005"} 950
diameter_client_response_delay_seconds_bucket{le="0.01"} 990
diameter_client_response_delay_seconds_bucket{le="0.05"} 1000
diameter_client_response_delay_seconds_bucket{le="+Inf"} 1000
diameter_client_response_delay_seconds_sum 3.0
diameter_client_response_delay_seconds_count 1000
"""
    assert parse_counter(after, M_ANSWERS) - parse_counter(before, M_ANSWERS) == 1000.0
    hist = histogram_delta_percentiles(
        parse_histogram(before, M_DELAY), parse_histogram(after, M_DELAY), [50, 90, 99]
    )
    assert hist["count"] == 1000
    assert abs(hist["avg_s"] - 0.003) < 1e-9
    # 50% -> target 500 -> first cum>=500 is le=0.005 ; 90% -> 900 -> 0.005 ; 99% -> 990 -> 0.01
    assert hist["pctl"][50] == 0.005, hist["pctl"]
    assert hist["pctl"][90] == 0.005, hist["pctl"]
    assert hist["pctl"][99] == 0.01, hist["pctl"]
    # docker stats parsing
    import tempfile
    import os

    fd, path = tempfile.mkstemp()
    with os.fdopen(fd, "w") as f:
        f.write("a-h2diagent 12.5% 40.0\na-h2diagent 30.0% 55.5\nb-h2diagent 5.0% 20.0\n")
    st = parse_docker_stats(path)
    os.unlink(path)
    assert st["a-h2diagent"]["cpu_peak_pct"] == 30.0
    assert st["a-h2diagent"]["mem_peak_mib"] == 55.5
    assert st["a-h2diagent"]["cpu_avg_pct"] == 21.2
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description="Analyze an h2diagent load run and append a JSONL record.")
    ap.add_argument("--selftest", action="store_true", help="run offline sanity checks and exit")
    ap.add_argument("--before", help="Prometheus text snapshot before the load")
    ap.add_argument("--after", help="Prometheus text snapshot after the load")
    ap.add_argument("--stats", help="docker stats sample file")
    ap.add_argument("--duration", type=float, default=0.0, help="load duration in seconds")
    ap.add_argument("--example", default="unknown")
    ap.add_argument("--cps", default="")
    ap.add_argument("--jsonl", action="store_true", help="emit one JSON line to stdout instead of the summary")
    a = ap.parse_args()

    if a.selftest:
        selftest()
        return 0

    if not (a.before and a.after):
        ap.error("--before and --after are required (unless --selftest)")

    record = build_record(a)
    if a.jsonl:
        # One machine-readable line to STDOUT (no file is written; redirect the
        # process output yourself if you want to keep it).
        print(json.dumps(record))
    else:
        print_summary(record)
    return 0


if __name__ == "__main__":
    sys.exit(main())
