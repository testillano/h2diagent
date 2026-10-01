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
# Every function accepts an optional [port] argument (overriding H2DIAHLP_METRICS_PORT)
# and prints a one-line description with -h.

#############
# VARIABLES #
#############
# Namespaced (H2DIAHLP_*) and EXPORTED so a child process that runs (not sources)
# a script inheriting the exported functions also gets these variables. The
# per-helper prefix avoids clashing with the h2agent helper (H2AHLP_*) or any
# unrelated shell variable. Override via the namespaced name, e.g.
# H2DIAHLP_METRICS_PORT=9090 source helpers.bash
PNAME=${PNAME:-h2diagent}
H2DIAHLP_METRICS_PORT=${H2DIAHLP_METRICS_PORT:-8085}
H2DIAHLP_SCHEME=${H2DIAHLP_SCHEME:-http}
H2DIAHLP_SERVER_ADDR=${H2DIAHLP_SERVER_ADDR:-localhost}
export PNAME H2DIAHLP_METRICS_PORT H2DIAHLP_SCHEME H2DIAHLP_SERVER_ADDR
# NOTE: h2diagent exposes only a plain HTTP/1 Prometheus endpoint (no HTTP/2
# admin API), so its scrapes use `curl -s` inline. We deliberately do NOT define
# a global CURL variable here: exporting one would leak into a shell that also
# sources the h2agent helpers (which need `curl --http2-prior-knowledge` for the
# admin API), silently breaking their admin calls. Keep the curl inline.

#############
# FUNCTIONS #
#############

# -----------------------------------------------------------------------------
# INTERNAL

# metrics_url [port]: build the Prometheus scrape URL.
metrics_url() {
  local port="${1:-${H2DIAHLP_METRICS_PORT}}"
  echo "${H2DIAHLP_SCHEME}://${H2DIAHLP_SERVER_ADDR}:${port}/metrics"
}

# -----------------------------------------------------------------------------
# PUBLIC

# metrics [port]: raw Prometheus metrics scraped from h2diagent.
metrics() {
  [ "$1" = "-h" ] && { echo "metrics [port]: raw Prometheus metrics from h2diagent (default ${H2DIAHLP_METRICS_PORT})."; return 0; }
  curl -s "$(metrics_url "$1")" 2>/dev/null
}

# metrics_summary: Diameter client summary (result-codes, PASS(2001), latency)
# from h2diagent prometheus metrics, with an h2agent-like snapshot/delta command
# line (--now, --delta, --save, --last, --show, --clean, <ref1> <ref2>, --json).
# The core is the two-ref "delta mode"; every other mode funnels into it. A
# 'zeroed' baseline yields absolute totals (live view = "metrics_summary --now").
# Latency comes from the diameter_client_response_delay_seconds histogram
# (_sum/_count); the "By label" table also shows optional additional labels
# (e.g. cc_request_type) configured via --metrics-additional-label. It also
# renders a "by label x result-code" cross-tab (e.g. which cc_request_type got
# which result-code). The metrics port is taken from H2DIAHLP_METRICS_PORT (override
# with --port). Portable awk.
metrics_summary() {
  local snap_dir="/tmp/.h2diagent_metrics_summaries"

  # Colors (disabled if not a terminal)
  local C_RST="" C_BLD="" C_GRN="" C_RED="" C_CYN="" C_YLW="" C_MAG=""
  if [ -t 1 ]; then
    C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_GRN=$'\033[32m'; C_RED=$'\033[31m'
    C_CYN=$'\033[36m'; C_YLW=$'\033[33m'; C_MAG=$'\033[35m'
  fi

  # Optional --port <p>. Local (dynamic scope) so recursive calls below inherit
  # it while the caller's H2DIAHLP_METRICS_PORT stays untouched after we return.
  local H2DIAHLP_METRICS_PORT="${H2DIAHLP_METRICS_PORT:-}"
  local _args=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --port) H2DIAHLP_METRICS_PORT="$2"; shift 2 ;;
      *) _args+=("$1"); shift ;;
    esac
  done
  set -- "${_args[@]}"

  if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: metrics_summary [-h] [--port <p>] [--show] [--clean] [--json] [<ref1> <ref2>]"
    echo "       Metrics counters summary of the h2diagent prometheus endpoint, with"
    echo "       snapshot/delta support. Covers Diameter client (external), Diameter server"
    echo "       (inbound), and the internal HTTP/2 legs towards h2agent. Counters only:"
    echo "       gauges/histograms are dumped in snapshots but not summarised (use Grafana)."
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
    echo "       --port <p>         Metrics port (default: H2DIAHLP_METRICS_PORT=${H2DIAHLP_METRICS_PORT})."
    echo
    echo "       References can be unix timestamps or labels (order does not matter)."
    echo "       'last' is also usable as a ref. Reserved labels: 'zeroed', 'last'."
    echo "       Snapshots: ${snap_dir}/counters.<unix_ts>  Labels: ${snap_dir}/labels"
    echo
    echo "       Live view since start:  metrics_summary --now"
    echo "       Test window:            metrics_summary --save before; ...; metrics_summary before --now"
    echo "       Periodic increments:    while true; do metrics_summary --delta; sleep 60; done"
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
    metrics_summary "${last_ref}" last ${json_flag}
    return $?
  fi

  # --delta -> save snapshot + delta from previous last to the new snapshot
  if [ "$1" = "--delta" ]; then
    local json_flag=""; [ "$2" = "--json" ] && json_flag="--json"
    local prev_resolved=$(_resolve_ref "last") prev_ref="zeroed"
    [ -n "$prev_resolved" ] && prev_ref="${prev_resolved##* }"
    metrics_summary > /dev/null
    metrics_summary "${prev_ref}" last ${json_flag}
    return $?
  fi

  # --now -> ephemeral snapshot (live fetch, not persisted) + delta from ref to it
  local now_mode=false now_ref="zeroed" now_json=""
  local now_args=()
  for a in "$@"; do case "$a" in --now) now_mode=true ;; --json) now_json="--json" ;; *) now_args+=("$a") ;; esac; done
  if $now_mode; then
    [ ${#now_args[@]} -gt 0 ] && now_ref="${now_args[0]}"
    local m=$(curl -s "$(metrics_url)" 2>/dev/null)
    if [ -z "$m" ]; then echo "No metrics available (is h2diagent running?)"; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    local resolved=$(_resolve_ref "${now_ref}")
    if [ -z "$resolved" ]; then echo "Reference '${now_ref}' not found."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    local ts_now=$(date +%s) eph_name=""
    eph_name="${snap_dir}/counters.${ts_now}"
    echo "$m" | grep -E '_counter\{|_gauge\{|_bucket\{|_sum\{|_count\{' > "${eph_name}"
    metrics_summary "${now_ref}" "${ts_now}" ${now_json}
    local rc=$?
    rm -f "${eph_name}"
    return $rc
  fi

  # Save snapshot (no args, or --save <label>)
  if [ $# -eq 0 ] || [ "$1" = "--save" ]; then
    local label=""
    if [ "$1" = "--save" ]; then
      label=$2
      if [ -z "$label" ]; then echo "Usage: metrics_summary --save <label>"; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
      if [ "$label" = "zeroed" ] || [ "$label" = "last" ]; then echo "Error: '${label}' is a reserved label."; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi
    fi
    local m=$(curl -s "$(metrics_url)" 2>/dev/null)
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
  if [ ${#args[@]} -ne 2 ]; then metrics_summary -h; unset -f _resolve_ref _label_for_ts _fmt_ref; return 1; fi

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
      -v JSON="$json" -v HDR="$header" -v PORT="$H2DIAHLP_METRICS_PORT" \
      -v TS1="$ts1" -v TS2="$ts2" -v F1="$f1" '
    function getlabel(line, key,   re, s) {
      re = key "=\"[^\"]*\""
      if (match(line, re)) { s = substr(line, RSTART, RLENGTH); sub(key "=\"", "", s); sub(/"$/, "", s); return s }
      return ""
    }
    function cmdName(cc) {
      # Diameter command-code -> abbreviation (request form). Shown next to the
      # numeric code in the "By label" table for readability.
      if (cc == "257") return "CER"; if (cc == "258") return "RAR"
      if (cc == "265") return "AAR"; if (cc == "271") return "ACR"
      if (cc == "272") return "CCR"; if (cc == "274") return "ASR"
      if (cc == "275") return "STR"; if (cc == "280") return "DWR"
      if (cc == "282") return "DPR"; if (cc == "300") return "SNR"
      return "?"
    }
    function cmdAnsName(cc) {
      # Answer form (ends in A): request XxR -> answer XxA. Same command-code.
      if (cc == "257") return "CEA"; if (cc == "258") return "RAA"
      if (cc == "265") return "AAA"; if (cc == "271") return "ACA"
      if (cc == "272") return "CCA"; if (cc == "274") return "ASA"
      if (cc == "275") return "STA"; if (cc == "280") return "DWA"
      if (cc == "282") return "DPA"; if (cc == "300") return "SNA"
      return "?"
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
    # Inbound (server-initiated by the peer, e.g. RAR): received ON the client
    # connection and answered by us. Same connection, opposite direction -- the
    # summary used to ignore it. Aggregate per command-code.
    /^diameter_client_requests_received_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      inReq[cc] += sgn*$2; inSeen[cc] = 1; inReqTot += sgn*$2
    }
    /^diameter_client_answers_sent_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      rc = getlabel($0, "result_code"); if (rc == "") rc = "?"
      inAns[cc SUBSEP rc] += sgn*$2; inSeen[cc] = 1; inRcSeen[cc SUBSEP rc] = 1; inAnsTot += sgn*$2
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
    # --- Diameter server (inbound: peer -> h2diagent acting as a Diameter server) ---
    /^diameter_server_requests_received_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      dsReqRecv[cc] += sgn*$2; dsSeen[cc] = 1; dsReqRecvTot += sgn*$2
    }
    /^diameter_server_answers_sent_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      rc = getlabel($0, "result_code"); if (rc == "") rc = "?"
      dsAnsSent[cc SUBSEP rc] += sgn*$2; dsSeen[cc] = 1; dsAnsSentTot += sgn*$2
    }
    # Bidirectional server leg: h2diagent-initiated requests sent on the server leg.
    /^diameter_server_requests_sent_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      dsReqSent[cc] += sgn*$2; dsSeen[cc] = 1; dsReqSentTot += sgn*$2
    }
    /^diameter_server_answers_received_counter/ {
      cc = getlabel($0, "command_code"); if (cc == "") cc = "?"
      rc = getlabel($0, "result_code"); if (rc == "") rc = "?"
      dsAnsRecv[cc SUBSEP rc] += sgn*$2; dsSeen[cc] = 1; dsAnsRecvTot += sgn*$2
    }
    # active_peers is a GAUGE (instantaneous, not cumulative): it is still dumped
    # into the snapshot by the scrape grep, but NOT summarised here -- a delta of a
    # gauge is not statistically meaningful. Gauges/histograms belong in Grafana.
    # --- HTTP/2 (internal: h2diagent <-> h2agent) ---
    /^h2diagent_http2_server_requests_received_counter/ {
      m = getlabel($0, "method"); if (m == "") m = "?"
      hsReqRecv[m] += sgn*$2; hsSeen[m] = 1; hsReqRecvTot += sgn*$2
    }
    /^h2diagent_http2_server_responses_sent_counter/ {
      m = getlabel($0, "method"); if (m == "") m = "?"
      sc = getlabel($0, "status_code"); if (sc == "") sc = "?"
      hsRespSent[m SUBSEP sc] += sgn*$2; hsSeen[m] = 1; hsRespSentTot += sgn*$2
      if (sc ~ /^2/) hsOk += sgn*$2; else hsNok += sgn*$2
    }
    /^h2diagent_http2_client_requests_sent_counter/ {
      m = getlabel($0, "method"); if (m == "") m = "?"
      hcReqSent[m] += sgn*$2; hcSeen[m] = 1; hcReqSentTot += sgn*$2
    }
    /^h2diagent_http2_client_responses_received_counter/ {
      m = getlabel($0, "method"); if (m == "") m = "?"
      sc = getlabel($0, "status_code"); if (sc == "") sc = "?"
      hcRespRecv[m SUBSEP sc] += sgn*$2; hcSeen[m] = 1; hcRespRecvTot += sgn*$2
      if (sc ~ /^2/) hcOk += sgn*$2; else hcNok += sgn*$2
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
        printf "]},"
        # Diameter server (inbound) leg
        printf "\"diameter_server\":{\"requests_received\":%d,\"answers_sent\":%d,\"requests_sent\":%d,\"answers_received\":%d},", dsReqRecvTot, dsAnsSentTot, dsReqSentTot, dsAnsRecvTot
        # HTTP/2 internal (towards h2agent)
        printf "\"http2_internal\":{\"server\":{\"requests_received\":%d,\"responses_sent\":%d,\"responses_2xx\":%d,\"responses_non_2xx\":%d},\"client\":{\"requests_sent\":%d,\"responses_received\":%d,\"responses_2xx\":%d,\"responses_non_2xx\":%d}}", hsReqRecvTot, hsRespSentTot, hsOk, hsNok, hcReqSentTot, hcRespRecvTot, hcOk, hcNok
        printf "}"
        printf "\n"
        exit
      }
      printf "%s=== Metrics counters summary (h2diagent :%s) ===%s\n", BLD, PORT, RST
      printf "  %s(counters only; gauges/histograms are instantaneous -- see Grafana for those)%s\n", YLW, RST
      printf "  window: %s\n", HDR
      printf "\n%s--- Diameter client (external, towards SUT) ---%s\n", CYN, RST
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
      # Inbound (server-initiated by peer on the client connection): RAR/RAA etc.
      if (inReqTot != 0 || inAnsTot != 0) {
        printf "\n%s--- Inbound (server-initiated by peer: RAR, ...) ---%s\n", CYN, RST
        printf "  Received requests:\n"
        # order command-codes numerically so Received and Answers match
        _n = 0; for (cc in inSeen) { _ccs[++_n] = cc }
        for (_a = 1; _a <= _n; _a++) for (_b = _a+1; _b <= _n; _b++) if ((_ccs[_a]+0) > (_ccs[_b]+0)) { _t = _ccs[_a]; _ccs[_a] = _ccs[_b]; _ccs[_b] = _t }
        for (_a = 1; _a <= _n; _a++) { cc = _ccs[_a]; if (inReq[cc] == 0) continue; printf "    %-6s %-5s : %d\n", cc, "(" cmdName(cc) ")", inReq[cc] }
        printf "  Answers sent (by result-code):\n"
        for (_a = 1; _a <= _n; _a++) { cc = _ccs[_a]; for (kk in inAns) { split(kk, a, SUBSEP); if (a[1] != cc || inAns[kk] == 0) continue; col = (a[2] == "2001") ? GRN : RED; printf "    %-6s %-5s rc=%s%-6s%s : %d\n", a[1], "(" cmdAnsName(a[1]) ")", col, a[2], RST, inAns[kk] } }
      }
      if (totcount > 0) {
        printf "\n  %sLatency (s):%s\n", BLD, RST
        printf "    Average:     %.6f  (sum=%.3f count=%d)\n", totsum/totcount, totsum, totcount
        printf "    By label:\n"
        printf "      %s%-12s %-10s %-12s %-30s %-10s %-10s%s\n", BLD, "SOURCE", "APP", "COMMAND", "LABELS", "COUNT", "AVG(s)", RST
        for (k in seen) {
          c = lcount[k]; if (c <= 0) continue
          avg = lsum[k] / c
          lbl = (lext[k] == "" ? "-" : lext[k])
          printf "      %-12s %-10s %-12s %-30s %-10d %-10.6f\n", lsrc[k], lapp[k], lcc[k] "(" cmdName(lcc[k]) ")", lbl, c, avg
        }
      }
      # --- Diameter server (inbound: peer -> h2diagent as a Diameter server) ---
      if (dsReqRecvTot != 0 || dsAnsSentTot != 0 || dsReqSentTot != 0 || dsAnsRecvTot != 0) {
        printf "\n%s--- Diameter server (inbound, h2diagent as server) ---%s\n", CYN, RST
        # command-codes seen on the server leg, numerically ordered
        _dn = 0; for (cc in dsSeen) { _dcs[++_dn] = cc }
        for (_a = 1; _a <= _dn; _a++) for (_b = _a+1; _b <= _dn; _b++) if ((_dcs[_a]+0) > (_dcs[_b]+0)) { _t = _dcs[_a]; _dcs[_a] = _dcs[_b]; _dcs[_b] = _t }
        if (dsReqRecvTot != 0) {
          printf "  Requests received:\n"
          for (_a = 1; _a <= _dn; _a++) { cc = _dcs[_a]; if (dsReqRecv[cc] == 0) continue; printf "    %-6s %-5s : %d\n", cc, "(" cmdName(cc) ")", dsReqRecv[cc] }
        }
        if (dsAnsSentTot != 0) {
          printf "  Answers sent (by result-code):\n"
          for (_a = 1; _a <= _dn; _a++) { cc = _dcs[_a]; for (kk in dsAnsSent) { split(kk, a, SUBSEP); if (a[1] != cc || dsAnsSent[kk] == 0) continue; col = (a[2] == "2001") ? GRN : RED; printf "    %-6s %-5s rc=%s%-6s%s : %d\n", a[1], "(" cmdAnsName(a[1]) ")", col, a[2], RST, dsAnsSent[kk] } }
        }
        # Bidirectional (server-initiated by h2diagent on the server leg)
        if (dsReqSentTot != 0) {
          printf "  Requests sent (server-initiated):\n"
          for (_a = 1; _a <= _dn; _a++) { cc = _dcs[_a]; if (dsReqSent[cc] == 0) continue; printf "    %-6s %-5s : %d\n", cc, "(" cmdName(cc) ")", dsReqSent[cc] }
        }
        if (dsAnsRecvTot != 0) {
          printf "  Answers received (by result-code):\n"
          for (_a = 1; _a <= _dn; _a++) { cc = _dcs[_a]; for (kk in dsAnsRecv) { split(kk, a, SUBSEP); if (a[1] != cc || dsAnsRecv[kk] == 0) continue; col = (a[2] == "2001") ? GRN : RED; printf "    %-6s %-5s rc=%s%-6s%s : %d\n", a[1], "(" cmdAnsName(a[1]) ")", col, a[2], RST, dsAnsRecv[kk] } }
        }
      }
      # --- HTTP/2 (internal: h2diagent <-> h2agent) ---
      if (hsReqRecvTot != 0 || hsRespSentTot != 0 || hcReqSentTot != 0 || hcRespRecvTot != 0) {
        printf "\n%s--- HTTP/2 (internal, towards h2agent) ---%s\n", CYN, RST
        if (hsReqRecvTot != 0 || hsRespSentTot != 0) {
          printf "  Server leg (triggers received from h2agent):\n"
          for (m in hsSeen) { if (hsReqRecv[m] != 0) printf "    %-6s requests received : %d\n", m, hsReqRecv[m] }
          for (kk in hsRespSent) { if (hsRespSent[kk] == 0) continue; split(kk, a, SUBSEP); col = (a[2] ~ /^2/) ? GRN : RED; printf "    %-6s responses sent    : %s%s%s : %d\n", a[1], col, a[2], RST, hsRespSent[kk] }
          if (hsReqRecvTot > 0) printf "    %s2xx: %.1f%% (%d/%d)%s\n", MAG, 100*hsOk/(hsOk+hsNok > 0 ? hsOk+hsNok : 1), hsOk, hsOk+hsNok, RST
        }
        if (hcReqSentTot != 0 || hcRespRecvTot != 0) {
          printf "  Client leg (translations sent to h2agent):\n"
          for (m in hcSeen) { if (hcReqSent[m] != 0) printf "    %-6s requests sent     : %d\n", m, hcReqSent[m] }
          for (kk in hcRespRecv) { if (hcRespRecv[kk] == 0) continue; split(kk, a, SUBSEP); col = (a[2] ~ /^2/) ? GRN : RED; printf "    %-6s responses received: %s%s%s : %d\n", a[1], col, a[2], RST, hcRespRecv[kk] }
          if (hcRespRecvTot != 0) printf "    %s2xx: %.1f%% (%d/%d)%s\n", MAG, 100*hcOk/(hcOk+hcNok > 0 ? hcOk+hcNok : 1), hcOk, hcOk+hcNok, RST
        }
      }
    }' "$f1" "$f2"

  unset -f _resolve_ref _label_for_ts _fmt_ref
  return 0
}

help() {
  [ "${1:-}" = "-h" -o "${1:-}" = "--help" ] && echo "Usage: help; This help summary." && return 0
  echo
  echo "===== ${PNAME} metric helpers ====="
  echo "Metrics & monitoring: https://github.com/testillano/h2diagent#metrics-and-monitoring"
  echo "Usage: help; This help summary."
  echo
  echo "=== Internal Functions And Variables ==="
  echo "metrics_url: $(metrics_url) (H2DIAHLP_METRICS_PORT=${H2DIAHLP_METRICS_PORT}; H2DIAHLP_SCHEME=${H2DIAHLP_SCHEME}; H2DIAHLP_SERVER_ADDR=${H2DIAHLP_SERVER_ADDR})"
  export -f metrics_url
  echo
  echo "=== Functions ==="
  for f in metrics metrics_summary; do ${f} -h | head -n 1; export -f ${f} ; done
  echo
}

#############
# EXECUTION #
#############

# Check dependencies: warn only, do NOT 'return'. The functions are already
# defined above, so a 'return' here would NOT prevent their use (it only aborts
# the sourcing shell -- breaking scripts that source this as a library) while
# giving no real protection: a function needing a missing tool fails on use
# anyway. So we just warn (to stderr) and keep the library sourceable.
# h2diagent uses curl (scrapes) and awk (metrics_summary parsing/formatting);
# it does NOT use jq/python3. Ubiquitous coreutils (sed/grep/sort/date) assumed.
for _dep in curl awk; do
  type "${_dep}" &>/dev/null || echo "WARNING: missing dependency '${_dep}' -- some helper functions will fail until it is installed." >&2
done
unset _dep

# Show help
help
