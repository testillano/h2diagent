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

# traffic_summary [port]: Diameter client summary (result-codes, PASS, latency)
# with an h2agent-like layout and colors. Live snapshot (no delta machinery).
# Latency comes from the diameter_client_response_delay_seconds histogram
# (_sum/_count); the "By label" table also shows the optional additional label
# (e.g. cc_request_type) when configured via --metrics-additional-label.
# Portable awk (mawk/gawk).
traffic_summary() {
  [ "$1" = "-h" ] && { echo "traffic_summary [port]: Diameter client result-codes, PASS(2001) and latency (default ${METRICS_PORT})."; return 0; }
  local port="${1:-${METRICS_PORT}}"

  local C_RST="" C_BLD="" C_GRN="" C_RED="" C_CYN="" C_YLW="" C_MAG=""
  if [ -t 1 ]; then
    C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_GRN=$'\033[32m'; C_RED=$'\033[31m'
    C_CYN=$'\033[36m'; C_YLW=$'\033[33m'; C_MAG=$'\033[35m'
  fi

  ${CURL} "$(metrics_url "${port}")" 2>/dev/null | awk \
    -v PORT="${port}" -v RST="$C_RST" -v BLD="$C_BLD" -v GRN="$C_GRN" -v RED="$C_RED" \
    -v CYN="$C_CYN" -v YLW="$C_YLW" -v MAG="$C_MAG" '
    function getlabel(line, key,   re, s) {
      re = key "=\"[^\"]*\""
      if (match(line, re)) { s = substr(line, RSTART, RLENGTH); sub(key "=\"", "", s); sub(/"$/, "", s); return s }
      return ""
    }
    # join of all labels that are not base (source/command_code/application_id/
    # result_code/le) as "k=v,k2=v2" (empty if none).
    function extras(line,   body, n, arr, i, k, v, p, out) {
      if (!match(line, /\{[^}]*\}/)) return ""
      body = substr(line, RSTART+1, RLENGTH-2)
      n = split(body, arr, ",")
      out = ""
      for (i = 1; i <= n; i++) {
        p = index(arr[i], "="); if (p == 0) continue
        k = substr(arr[i], 1, p-1); v = substr(arr[i], p+1); gsub(/"/, "", v)
        if (k == "source" || k == "command_code" || k == "application_id" || k == "result_code" || k == "le") continue
        out = (out == "" ? "" : out ",") k "=" v
      }
      return out
    }
    /^diameter_client_requests_sent_counter/     { sent += $NF }
    /^diameter_client_requests_unsent_counter/   { unsent += $NF }
    /^diameter_client_requests_timedout_counter/ { tmo += $NF }
    /^diameter_client_answers_received_counter/ {
      v = $NF; recv += v
      rc = getlabel($0, "result_code"); if (rc == "") rc = "?"
      cnt[rc] += v
      if (rc == "2001") ok += v; else nok += v
    }
    /^diameter_client_response_delay_seconds_sum/ {
      src = getlabel($0, "source"); app = getlabel($0, "application_id"); cc = getlabel($0, "command_code")
      ex = extras($0); key = src SUBSEP app SUBSEP cc SUBSEP ex
      lsum[key] += $NF; seen[key] = 1; lsrc[key] = src; lapp[key] = app; lcc[key] = cc; lext[key] = ex
      totsum += $NF
    }
    /^diameter_client_response_delay_seconds_count/ {
      src = getlabel($0, "source"); app = getlabel($0, "application_id"); cc = getlabel($0, "command_code")
      ex = extras($0); key = src SUBSEP app SUBSEP cc SUBSEP ex
      lcount[key] += $NF; seen[key] = 1
      totcount += $NF
    }
    END {
      printf "%s=== Diameter client traffic summary (h2diagent :%s) ===%s\n", BLD, PORT, RST
      printf "\n%s--- Client ---%s\n", CYN, RST
      printf "  Sent:          %d\n", sent
      printf "  Unsent:        %d\n", unsent
      printf "  Received:      %d (2001:%d non-2001:%d)\n", recv, ok, nok
      printf "  Timeouts:      %d\n", tmo
      printf "  ---\n"
      if (recv > 0)
        printf "  %sPASS 2001: %.1f%% (%d/%d)%s\n", MAG, 100*ok/recv, ok, recv, RST
      else
        printf "  %s(no answers yet)%s\n", YLW, RST
      if (recv > 0) {
        printf "\n  by result-code:\n"
        for (rc in cnt) {
          col = (rc == "2001") ? GRN : RED
          printf "    %s%-8s%s : %d\n", col, rc, RST, cnt[rc]
        }
      }
      if (totcount > 0) {
        printf "\n  %sLatency (s):%s\n", BLD, RST
        printf "    Average:     %.6f  (sum=%.3f count=%d)\n", totsum/totcount, totsum, totcount
        printf "    By label:\n"
        printf "      %s%-12s %-10s %-8s %-30s %-10s %-10s%s\n", BLD, "SOURCE", "APP", "COMMAND", "LABELS", "COUNT", "AVG(s)", RST
        for (k in seen) {
          c = lcount[k]; if (c == 0) continue
          avg = lsum[k] / c
          lbl = (lext[k] == "" ? "-" : lext[k])
          printf "      %-12s %-10s %-8s %-30s %-10d %-10.6f\n", lsrc[k], lapp[k], lcc[k], lbl, c, avg
        }
      }
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
