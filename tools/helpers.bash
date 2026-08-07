#!/bin/echo "source me!"
#
# h2diagent helper functions (metrics shortcuts).
#
# This is the h2diagent project, so these helpers cover h2diagent only. The
# h2agent sidecar belongs to another project and ships its own helpers in its
# own container -- they are NOT here. Function names are kept simple; if you
# source both this and the h2agent helpers in the same shell, resolve any name
# collisions there.
#
# Usage:
#   - Native:     source tools/helpers.bash   (localhost:8085 by default)
#   - Container:  mounted by the helm chart and sourced into ~/.bashrc for the
#                 h2diagent container (variables below are set from helm values).
#
# h2diagent has no administrative REST interface, so these are shortcuts over
# the Prometheus metrics endpoint (the real Diameter result codes live here:
# h2agent metrics only reflect HTTP/2 relay, where a Diameter error answer is
# still carried as HTTP/2 200).
#
# Every function accepts an optional [port] argument (overriding METRICS_PORT)
# and prints a one-line description with -h.

#############
# VARIABLES #
#############
PNAME=${PNAME:-h2diagent}
METRICS_PORT=${METRICS_PORT:-8085}
SCHEME=${SCHEME:-http}
SERVER_ADDR=${SERVER_ADDR:-localhost}
CURL=${CURL:-"curl -s"}

#############
# FUNCTIONS #
#############

# -----------------------------------------------------------------------------
# INTERNAL

# metrics_url [port]: build the Prometheus scrape URL.
metrics_url() {
  local port="${1:-${METRICS_PORT}}"
  echo "${SCHEME}://${SERVER_ADDR}:${port}/metrics"
}

# -----------------------------------------------------------------------------
# PUBLIC

# metrics [port]: raw Prometheus metrics scraped from h2diagent.
metrics() {
  [ "$1" = "-h" ] && { echo "metrics [port]: raw Prometheus metrics from h2diagent (default ${METRICS_PORT})."; return 0; }
  ${CURL} "$(metrics_url "$1")" 2>/dev/null
}

# traffic_summary [port]: Diameter client answers by result-code, with
# pass (2001) / fail (non-2001) breakdown. Portable awk (mawk/gawk).
traffic_summary() {
  [ "$1" = "-h" ] && { echo "traffic_summary [port]: Diameter answers by result-code, pass(2001)/fail (default ${METRICS_PORT})."; return 0; }
  local port="${1:-${METRICS_PORT}}"
  ${CURL} "$(metrics_url "${port}")" 2>/dev/null | awk -v PORT="${port}" '
    /^diameter_client_requests_sent_counter/ { sent += $NF }
    /^diameter_client_answers_received_counter/ {
      v = $NF; recv += v
      rc = "?"
      if (match($0, /result_code="[0-9]+"/)) {
        s = substr($0, RSTART, RLENGTH)   # e.g. result_code="2001"
        gsub(/[^0-9]/, "", s)
        rc = s
      }
      cnt[rc] += v
      if (rc == "2001") ok += v; else nok += v
    }
    END {
      printf "=== Diameter client traffic summary (h2diagent :%s) ===\n", PORT
      printf "  requests sent:    %d\n", sent
      printf "  answers received: %d\n", recv
      if (recv == 0) { print "  (no answers yet)"; exit }
      printf "  by result-code:\n"
      for (rc in cnt) printf "    %-8s : %d\n", rc, cnt[rc]
      printf "  PASS 2001:        %d (%.1f%%)\n", ok, 100 * ok / recv
      printf "  FAIL non-2001:    %d (%.1f%%)\n", nok, 100 * nok / recv
    }'
}

help() {
  [ "$1" = "-h" -o "$1" = "--help" ] && echo "Usage: help; This help summary." && return 0
  echo
  echo "===== ${PNAME} metric helpers ====="
  echo "Metrics & monitoring: https://github.com/testillano/h2diagent#metrics-and-monitoring"
  echo "Usage: help; This help summary."
  echo
  echo "=== Internal Functions And Variables ==="
  echo "metrics_url: $(metrics_url) (METRICS_PORT=${METRICS_PORT}; SCHEME=${SCHEME}; SERVER_ADDR=${SERVER_ADDR})"
  echo "curl:        CURL=\"${CURL}\""
  export -f metrics_url
  echo
  echo "=== Functions ==="
  for f in metrics traffic_summary; do ${f} -h | head -n 1; export -f ${f} ; done
  echo
}

#############
# EXECUTION #
#############

# Check dependencies:
if ! type curl &>/dev/null; then echo "Missing required dependency (curl) !" ; return 1 ; fi

# Show help
help
