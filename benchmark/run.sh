#!/bin/bash
# =============================================================================
# run.sh - h2diagent load benchmark (reuses the examples/ deployment tooling)
# =============================================================================
# Deploys an example (default: gx-high-load), fires its configured trigger,
# samples container CPU/memory while the load runs, then scrapes the client
# gateway Prometheus metrics and prints a report (nothing is written to disk;
# capture stdout yourself if you want to keep it).
#
# It is a FIXED-LOAD reference: the example pins the request count and CPS, so
# the meaningful before/after comparison is latency + CPU/memory at that load
# (throughput tracks the offered CPS unless the gateway saturates).
#
# Usage:
#   ./run.sh [example] [--jsonl] [--timeout SECONDS] [--interval SECONDS]
#            [--no-teardown]
#
# Requires: docker, docker-compose, curl, python3, awk (all already used by the
# project tooling). Run from anywhere; paths are resolved relative to this file.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EXAMPLES_DIR="${PROJECT_DIR}/examples"
DEPLOY_DIR="${EXAMPLES_DIR}/deployment"

EXAMPLE="gx-high-load"
JSONL=0
TIMEOUT=120
INTERVAL=1
TEARDOWN=1

M_ANSWERS="diameter_client_answers_received_counter"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jsonl) JSONL=1; shift ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --interval) INTERVAL="$2"; shift 2 ;;
        --no-teardown) TEARDOWN=0; shift ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) EXAMPLE="$1"; shift ;;
    esac
done

WORK="$(mktemp -d)"
BEFORE="${WORK}/before.txt"
AFTER="${WORK}/after.txt"
STATS="${WORK}/stats.txt"
SAMPLER_FLAG="${WORK}/sampling"
SAMPLER_PID=""

cleanup() {
    [ -n "${SAMPLER_PID}" ] && kill "${SAMPLER_PID}" 2>/dev/null
    rm -f "${SAMPLER_FLAG}" 2>/dev/null
    if [ "${TEARDOWN}" = "1" ]; then
        echo "Tearing down..."
        ( cd "${EXAMPLES_DIR}" && ./5.stop.sh ) >/dev/null 2>&1 || true
    fi
    rm -rf "${WORK}" 2>/dev/null
}
trap cleanup EXIT

scrape() {  # scrape <port> <outfile>
    curl -sf "http://localhost:$1/metrics" -o "$2" 2>/dev/null || : > "$2"
}

count_answers() {  # count_answers <port>
    curl -sf "http://localhost:$1/metrics" 2>/dev/null \
        | awk -v n="${M_ANSWERS}" 'index($0,n)==1 { s += $NF } END { printf "%d", s+0 }'
}

# docker stats sampler: appends "<name> <cpu%> <memMiB>" lines until flag is gone.
sampler() {
    while [ -f "${SAMPLER_FLAG}" ]; do
        docker stats --no-stream --format '{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}' "${DIA_CONTAINERS[@]}" 2>/dev/null \
            | awk -F'|' '{
                name=$1; cpu=$2; sub(/%/,"",cpu);
                split($3, a, " "); used=a[1];
                num=0; u="";
                if (match(used, /[0-9.]+/)) { num=substr(used,RSTART,RLENGTH); u=substr(used,RSTART+RLENGTH) }
                mib=num;
                if (u ~ /GiB/) mib=num*1024;
                else if (u ~ /MiB/) mib=num;
                else if (u ~ /KiB/) mib=num/1024;
                else if (u ~ /B/) mib=num/1048576;
                printf "%s %s %.2f\n", name, cpu, mib
              }' >> "${STATS}"
        sleep "${INTERVAL}"
    done
}

# --- Deploy + run ----------------------------------------------------------
echo "=== h2diagent load benchmark: ${EXAMPLE} ==="
( cd "${EXAMPLES_DIR}" && ./1.deploy.sh "${EXAMPLE}" ) || { echo "deploy failed"; exit 1; }
( cd "${EXAMPLES_DIR}" && ./2.run.sh ) || { echo "run failed"; exit 1; }

[ -f "${DEPLOY_DIR}/config.bash" ] || { echo "no deployment config"; exit 1; }
# shellcheck disable=SC1091
source "${DEPLOY_DIR}/config.bash"

# --- Resolve client peer (metrics source) + container list -----------------
CLIENT_ADMIN=""
DIA_CONTAINERS=()
for peer_def in "${PEERS[@]}"; do
    read -r NAME ROLE _REST <<< "${peer_def}"
    DIA_CONTAINERS+=("${NAME}-h2diagent")
    if [ "${ROLE}" = "client" ] || [ "${ROLE}" = "both" ]; then
        ap="$(echo "${peer_def}" | grep -oE -- '--admin-port[= ]+[0-9]+' | grep -oE '[0-9]+' | head -1)"
        [ -n "${ap}" ] && CLIENT_ADMIN="${ap}"
    fi
done
if [ -z "${CLIENT_ADMIN}" ]; then
    echo "Could not determine client admin port from deployment PEERS"; exit 1
fi
CLIENT_PROM=$((CLIENT_ADMIN + 3000))
echo "Client gateway metrics: http://localhost:${CLIENT_PROM}/metrics"

# --- Expected count + CPS from the trigger ---------------------------------
CPS="$(echo "${TRIGGER:-}" | grep -oE 'cps=[0-9]+' | grep -oE '[0-9]+' | head -1)"
SEQ_END="$(echo "${TRIGGER:-}" | grep -oE 'sequenceEnd=[0-9]+' | grep -oE '[0-9]+' | head -1)"
EXPECTED=""
[ -n "${SEQ_END}" ] && EXPECTED=$((SEQ_END + 1))
echo "Trigger: cps=${CPS:-?} expected_answers=${EXPECTED:-unknown}"

# --- Baseline scrape + start sampler ---------------------------------------
scrape "${CLIENT_PROM}" "${BEFORE}"
touch "${SAMPLER_FLAG}"
sampler & SAMPLER_PID=$!

# --- Fire the load ---------------------------------------------------------
START=$(date +%s.%N)
START_EPOCH=$(date +%s)
( cd "${EXAMPLES_DIR}" && ./4.trigger.sh ) || { echo "trigger failed"; exit 1; }

# --- Wait for completion (target count, or plateau) ------------------------
prev=-1; stable=0; elapsed=0
while :; do
    now=$(count_answers "${CLIENT_PROM}")
    if [ -n "${EXPECTED}" ] && [ "${now}" -ge "${EXPECTED}" ]; then
        break
    fi
    if [ "${now}" = "${prev}" ]; then
        stable=$((stable + 1))
    else
        stable=0
    fi
    # Plateau: no progress for ~5s and at least some answers seen.
    if [ -z "${EXPECTED}" ] && [ "${stable}" -ge 5 ] && [ "${now}" -gt 0 ]; then
        break
    fi
    prev="${now}"
    elapsed=$(( $(date +%s) - START_EPOCH ))
    if [ "${elapsed}" -ge "${TIMEOUT}" ]; then
        echo "WARNING: timeout after ${TIMEOUT}s (answers=${now}, expected=${EXPECTED:-?})"
        break
    fi
    sleep "${INTERVAL}"
done
END=$(date +%s.%N)
DURATION=$(awk -v a="${START}" -v b="${END}" 'BEGIN{ printf "%.3f", b - a }')

# --- Stop sampler + final scrape -------------------------------------------
rm -f "${SAMPLER_FLAG}"
[ -n "${SAMPLER_PID}" ] && wait "${SAMPLER_PID}" 2>/dev/null
SAMPLER_PID=""
scrape "${CLIENT_PROM}" "${AFTER}"

# --- Report ----------------------------------------------------------------
python3 "${SCRIPT_DIR}/report.py" \
    --before "${BEFORE}" --after "${AFTER}" --stats "${STATS}" \
    --duration "${DURATION}" --example "${EXAMPLE}" --cps "${CPS:-}" \
    $([ "${JSONL}" = "1" ] && echo --jsonl)
