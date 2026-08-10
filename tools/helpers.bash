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

# traffic_summary: Diameter client summary (result-codes, PASS(2001), latency)
# from h2diagent prometheus metrics, with an h2agent-like snapshot/delta command
# line (--now, --delta, --save, --last, --show, --clean, <ref1> <ref2>, --json).
# The core is the two-ref "delta mode"; every other mode funnels into it. A
# 'zeroed' baseline yields absolute totals (live view = "traffic_summary --now").
# Latency comes from the diameter_client_response_delay_seconds histogram
# (_sum/_count); the "By label" table also shows optional additional labels
# (e.g. cc_request_type) configured via --metrics-additional-label. It also
# renders a "by label x result-code" cross-tab (e.g. which cc_request_type got
# which result-code). The metrics port is taken from METRICS_PORT (override
# with --port). Portable awk.
traffic_summary() {
  local snap_dir="/tmp/.h2diagent_traffic_summaries"

  # Colors (disabled if not a terminal)
  local C_RST="" C_BLD="" C_GRN="" C_RED="" C_CYN="" C_YLW="" C_MAG=""
  if [ -t 1 ]; then
    C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_GRN=$'\033[32m'; C_RED=$'\033[31m'
    C_CYN=$'\033[36m'; C_YLW=$'\033[33m'; C_MAG=$'\033[35m'
  fi

  # Optional --port <p>. Local (dynamic scope) so recursive calls below inherit
  # it while the caller's METRICS_PORT stays untouched after we return.
  local METRICS_PORT="${METRICS_PORT}"
  local _args=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --port) METRICS_PORT="$2"; shift 2 ;;
      *) _args+=("$1"); shift ;;
    esac
  done
  set -- "${_args[@]}"

  if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: traffic_summary [-h] [--port <p>] [--show] [--clean] [--json] [<ref1> <ref2>]"
    echo "       Diameter client traffic summary (result-codes, PASS(2001), latency)"
    echo "       from h2diagent prometheus metrics, with snapshot/delta support."
    echo
    echo "       (no args)          Save a counters snapshot labelled with timestamp."
    echo "       --save <label>     Save a snapshot with a label given by user."
    echo "       --delta            Save snapshot + delta from previous last to it."
    echo
    echo "       --now              Volatile snapshot (live fetch) + delta from zeroed to it."
    echo "       <ref> --now        Volatile snapshot (live fetch) + delta from <ref> to it."
    echo "       --last             Delta from zeroed to last saved snapshot (no fetch)."
    echo "       <ref1> <ref2>      Delta between two refs (timestamp or label)."
    echo
    echo "       --show             List saved snapshots and their labels."
    echo "       --clean            Remove all saved snapshots."
    echo "       --json             JSON output (combinable with --now/--last/--delta)."
    echo "       --port <p>         Metrics port (default: METRICS_PORT=${METRICS_PORT})."
    echo
    echo "       References can be unix timestamps or labels (order does not matter)."
    echo "       'last' is also usable as a ref. Reserved labels: 'zeroed', 'last'."
    echo "       Snapshots: ${snap_dir}/counters.<unix_ts>  Labels: ${snap_dir}/labels"
    echo
    echo "       Live view since start:  traffic_summary --now"
    echo "       Test window:            traffic_summary --save before; ...; traffic_summary before --now"
    echo "       Periodic increments:    while true; do traffic_summary --delta; sleep 60; done"
    return 0
  fi

  if [ "$1" = "--clean" ]; then
    rm -rf "${snap_dir}"
    echo "Snapshots removed."
    return 0
  fi

  mkdir -p "${snap_dir}"
  touch "${snap_dir}/labels"

  # Resolve a reference (label or timestamp) to "filepath timestamp".
  _resolve_ref() {
    local ref=$1
    if [ "$ref" = "zeroed" ]; then echo "/dev/null 0"; return 0; fi
    local ts=$(awk -F= -v l="$ref" '$1==l{print $2;exit}' "${snap_dir}/labels")
    if [ -n "$ts" ] && [ -f "${snap_dir}/counters.${ts}" ]; then echo "${snap_dir}/counters.${ts} ${ts}"; return 0; fi
    if [ -f "${snap_dir}/counters.${ref}" ]; then echo "${snap_dir}/counters.${ref} ${ref}"; return 0; fi
    return 1
  }
  _label_for_ts() { awk -F= -v t="$1" '$2==t{print $1;exit}' "${snap_dir}/labels"; }
  _fmt_ref() {
    local ts=$1
    if [ "$ts" = "0" ]; then printf "%szeroed%s" "$C_CYN" "$C_RST"; return; fi
    local lbl=$(_label_for_ts "$ts")
    local dt=$(date -d @${ts} '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
    if [ -n "$lbl" ]; then printf "%s%s%s (%s) %s" "$C_CYN" "$ts" "$C_RST" "$lbl" "$dt"
    else printf "%s%s%s %s" "$C_CYN" "$ts" "$C_RST" "$dt"; fi
  }

  # Show history
  if [ "$1" = "--show" ]; then
    local files=$(ls -1 "${snap_dir}"/counters.* 2>/dev/null | sort)
    if [ -z "$files" ]; then echo "No snapshots saved yet."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 0; fi
    printf "%s%-14s  %-12s  %s%s\n" "$C_BLD" "TIMESTAMP" "LABEL" "DATE/TIME" "$C_RST"
    for f in ${files}; do
      local t=${f##*.}; local lbl=$(_label_for_ts "$t")
      printf "%-14s  %-12s  %s\n" "${t}" "${lbl:--}" "$(date -d @${t} '+%Y-%m-%d %H:%M:%S')"
    done
    unset -f _resolve_ref _label_for_ts _fmt_ref; return 0
  fi

  # --last -> delta from ref (default zeroed) to last snapshot (no fetch)
  if [[ " $* " == *" --last "* ]] || [ "$1" = "--last" ]; then
    local json_flag="" last_ref="zeroed"
    for a in "$@"; do case "$a" in --last) ;; --json) json_flag="--json" ;; *) last_ref="$a" ;; esac; done
    local resolved=$(_resolve_ref "last")
    if [ -z "$resolved" ]; then echo "No snapshots saved yet."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 0; fi
    traffic_summary "${last_ref}" last ${json_flag}
    return $?
  fi

  # --delta -> save snapshot + delta from previous last to the new snapshot
  if [ "$1" = "--delta" ]; then
    local json_flag=""; [ "$2" = "--json" ] && json_flag="--json"
    local prev_resolved=$(_resolve_ref "last") prev_ref="zeroed"
    [ -n "$prev_resolved" ] && prev_ref="${prev_resolved##* }"
    traffic_summary > /dev/null
    traffic_summary "${prev_ref}" last ${json_flag}
    return $?
  fi

  # --now -> ephemeral snapshot (live fetch, not persisted) + delta from ref to it
  local now_mode=false now_ref="zeroed" now_json=""
  local now_args=()
  for a in "$@"; do case "$a" in --now) now_mode=true ;; --json) now_json="--json" ;; *) now_args+=("$a") ;; esac; done
  if $now_mode; then
    [ ${#now_args[@]} -gt 0 ] && now_ref="${now_args[0]}"
    local m=$(${CURL} "$(metrics_url)" 2>/dev/null)
    if [ -z "$m" ]; then echo "No metrics available (is h2diagent running?)"; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    local resolved=$(_resolve_ref "${now_ref}")
    if [ -z "$resolved" ]; then echo "Reference '${now_ref}' not found."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    local ts_now=$(date +%s) eph_name=""
    eph_name="${snap_dir}/counters.${ts_now}"
    echo "$m" | grep -E '_counter\{|_gauge\{|_bucket\{|_sum\{|_count\{' > "${eph_name}"
    traffic_summary "${now_ref}" "${ts_now}" ${now_json}
    local rc=$?
    rm -f "${eph_name}"
    return $rc
  fi

  # Save snapshot (no args, or --save <label>)
  if [ $# -eq 0 ] || [ "$1" = "--save" ]; then
    local label=""
    if [ "$1" = "--save" ]; then
      label=$2
      if [ -z "$label" ]; then echo "Usage: traffic_summary --save <label>"; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
      if [ "$label" = "zeroed" ] || [ "$label" = "last" ]; then echo "Error: '${label}' is a reserved label."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    fi
    local m=$(${CURL} "$(metrics_url)" 2>/dev/null)
    if [ -z "$m" ]; then echo "No metrics available (is h2diagent running?)"; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    local ts=$(date +%s)
    echo "$m" | grep -E '_counter\{|_gauge\{|_bucket\{|_sum\{|_count\{' > "${snap_dir}/counters.${ts}"
    sed -i "/^last=/d" "${snap_dir}/labels"; echo "last=${ts}" >> "${snap_dir}/labels"
    if [ -n "$label" ]; then sed -i "/^${label}=/d" "${snap_dir}/labels"; echo "${label}=${ts}" >> "${snap_dir}/labels"; fi
    printf "Snapshot saved: %s\n" "$(_fmt_ref "$ts")"
    unset -f _resolve_ref _label_for_ts _fmt_ref
    return 0
  fi

  # Delta mode: exactly two refs (older first; counters only grow).
  local json="false"; local args=()
  for a in "$@"; do [ "$a" = "--json" ] && json="true" || args+=("$a"); done
  if [ ${#args[@]} -ne 2 ]; then traffic_summary -h; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi

  local resolved1=$(_resolve_ref "${args[0]}")
  [ -z "$resolved1" ] && { echo "Reference '${args[0]}' not found."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; }
  local f1=${resolved1% *} ts1=${resolved1##* }
  local resolved2=$(_resolve_ref "${args[1]}")
  [ -z "$resolved2" ] && { echo "Reference '${args[1]}' not found."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; }
  local f2=${resolved2% *} ts2=${resolved2##* }

  if [ "$ts1" -gt "$ts2" ] 2>/dev/null; then
    local tf=$f1 tt=$ts1; f1=$f2; ts1=$ts2; f2=$tf; ts2=$tt
  fi

  local header="$(_fmt_ref "$ts1") ${C_BLD}->${C_RST} $(_fmt_ref "$ts2")"

  # Render as a delta f2 - f1. FILENAME distinguishes the baseline (robust even
  # when f1 is /dev/null, i.e. a 'zeroed' baseline -> absolute totals).
  awk -v RST="$C_RST" -v BLD="$C_BLD" -v GRN="$C_GRN" -v RED="$C_RED" \
      -v CYN="$C_CYN" -v YLW="$C_YLW" -v MAG="$C_MAG" \
      -v JSON="$json" -v HDR="$header" -v PORT="$METRICS_PORT" \
      -v TS1="$ts1" -v TS2="$ts2" -v F1="$f1" '
    function getlabel(line, key,   re, s) {
      re = key "=\"[^\"]*\""
      if (match(line, re)) { s = substr(line, RSTART, RLENGTH); sub(key "=\"", "", s); sub(/"$/, "", s); return s }
      return ""
    }
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
    { sgn = (FILENAME == F1) ? -1 : 1 }
    /^diameter_client_requests_sent_counter/     { sent += sgn*$2 }
    /^diameter_client_requests_unsent_counter/   { unsent += sgn*$2 }
    /^diameter_client_requests_timedout_counter/ { tmo += sgn*$2 }
    /^diameter_client_answers_received_counter/ {
      v = sgn*$2; recv += v
      rc = getlabel($0, "result_code"); if (rc == "") rc = "?"
      cnt[rc] += v; if (rc == "2001") ok += v; else nok += v
      exa = extras($0)
      if (exa != "") { xcnt[exa SUBSEP rc] += v; xseen[exa] = 1; rcseen[rc] = 1 }
    }
    /^diameter_client_response_delay_seconds_sum/ {
      src = getlabel($0, "source"); app = getlabel($0, "application_id"); cc = getlabel($0, "command_code")
      ex = extras($0); key = src SUBSEP app SUBSEP cc SUBSEP ex
      lsum[key] += sgn*$2; seen[key] = 1; lsrc[key] = src; lapp[key] = app; lcc[key] = cc; lext[key] = ex; totsum += sgn*$2
    }
    /^diameter_client_response_delay_seconds_count/ {
      src = getlabel($0, "source"); app = getlabel($0, "application_id"); cc = getlabel($0, "command_code")
      ex = extras($0); key = src SUBSEP app SUBSEP cc SUBSEP ex
      lcount[key] += sgn*$2; seen[key] = 1; totcount += sgn*$2
    }
    END {
      if (JSON == "true") {
        printf "{"
        printf "\"window\":{\"from\":%s,\"to\":%s},", TS1, TS2
        printf "\"sent\":%d,\"unsent\":%d,\"timeouts\":%d,\"received\":%d,\"ok_2001\":%d,\"non_2001\":%d,", sent, unsent, tmo, recv, ok, nok
        printf "\"by_result_code\":{"; fr = 1
        for (rc in cnt) { if (cnt[rc] == 0) continue; if (!fr) printf ","; printf "\"%s\":%d", rc, cnt[rc]; fr = 0 }
        printf "},"
        printf "\"latency\":{\"count\":%d,\"sum\":%.6f,\"avg\":%.6f,", totcount, totsum, (totcount > 0 ? totsum/totcount : 0)
        printf "\"by_label\":["; fr = 1
        for (k in seen) { c = lcount[k]; if (c <= 0) continue; if (!fr) printf ","; printf "{\"source\":\"%s\",\"application_id\":\"%s\",\"command_code\":\"%s\",\"labels\":\"%s\",\"count\":%d,\"avg\":%.6f}", lsrc[k], lapp[k], lcc[k], lext[k], c, lsum[k]/c; fr = 0 }
        printf "]}}"
        printf "\n"
        exit
      }
      printf "%s=== Diameter client traffic summary (h2diagent :%s) ===%s\n", BLD, PORT, RST
      printf "  window: %s\n", HDR
      printf "\n%s--- Client ---%s\n", CYN, RST
      printf "  Sent:          %d\n", sent
      printf "  Unsent:        %d\n", unsent
      printf "  Received:      %d (2001:%d non-2001:%d)\n", recv, ok, nok
      printf "  Timeouts:      %d\n", tmo
      printf "  ---\n"
      if (recv > 0)
        printf "  %sPASS 2001: %.1f%% (%d/%d)%s\n", MAG, 100*ok/recv, ok, recv, RST
      else
        printf "  %s(no answers in window)%s\n", YLW, RST
      if (recv > 0) {
        printf "\n  by result-code:\n"
        for (rc in cnt) { if (cnt[rc] == 0) continue; col = (rc == "2001") ? GRN : RED; printf "    %s%-8s%s : %d\n", col, rc, RST, cnt[rc] }
        if (length(xseen) > 0) {
          printf "\n  by label x result-code:\n"
          for (e in xseen) for (rc2 in rcseen) { c2 = xcnt[e SUBSEP rc2]; if (c2 == 0) continue; col2 = (rc2 == "2001") ? GRN : RED; printf "    %-26s %s%-8s%s : %d\n", e, col2, rc2, RST, c2 }
        }
      }
      if (totcount > 0) {
        printf "\n  %sLatency (s):%s\n", BLD, RST
        printf "    Average:     %.6f  (sum=%.3f count=%d)\n", totsum/totcount, totsum, totcount
        printf "    By label:\n"
        printf "      %s%-12s %-10s %-8s %-30s %-10s %-10s%s\n", BLD, "SOURCE", "APP", "COMMAND", "LABELS", "COUNT", "AVG(s)", RST
        for (k in seen) {
          c = lcount[k]; if (c <= 0) continue
          avg = lsum[k] / c
          lbl = (lext[k] == "" ? "-" : lext[k])
          printf "      %-12s %-10s %-8s %-30s %-10d %-10.6f\n", lsrc[k], lapp[k], lcc[k], lbl, c, avg
        }
      }
    }' "$f1" "$f2"

  unset -f _resolve_ref _label_for_ts _fmt_ref
  return 0
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
