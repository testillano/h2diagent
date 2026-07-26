#!/bin/bash
# =============================================================================
# create-peer.sh - Generate a Diameter peer configuration
# =============================================================================
# Creates a peers/<name>/ directory with everything needed to run a Diameter
# peer (h2diagent + h2agent sidecar via docker-compose).
#
# Usage:
#   ./tools/create-peer.sh              # interactive
#   ./tools/create-peer.sh --help       # show help
#
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
STACKS_DIR="${SCRIPT_DIR}/stacks"
AVAILABLE_STACKS=$(ls "${STACKS_DIR}"/*.json 2>/dev/null | xargs -I{} basename {} .json | tr '\n' ' ')

# Defaults
DEFAULT_DIAMETER_PORT=3868
DEFAULT_ADMIN_PORT=8074
DEFAULT_ORIGIN_REALM="example.com"
DEFAULT_H2DIAGENT_IMAGE="${H2DIAGENT_IMAGE:-ghcr.io/testillano/h2diagent:latest}"
DEFAULT_H2AGENT_IMAGE="${H2AGENT_IMAGE:-ghcr.io/testillano/h2agent:latest}"
DEFAULT_OUTPUT_DIR="${PROJECT_DIR}/peers"

# =============================================================================
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Generate a Diameter peer configuration under peers/<name>/.

Options:
  -h, --help              Show this help
  -n, --name <name>       Peer name (required in non-interactive mode)
  -s, --stacks <list>     Space-separated stacks (e.g., "gx rx")
  -r, --role <role>       Connection role: server, client, or both
  -o, --output-dir <dir>  Output directory (default: ./peers)
  --server-port <port>    Diameter server listen port (default: ${DEFAULT_DIAMETER_PORT})
  --peer-host <host>      Remote Diameter peer host (for client role)
  --peer-port <port>      Remote Diameter peer port (default: ${DEFAULT_DIAMETER_PORT})
  --admin-port <port>     h2agent admin port (default: ${DEFAULT_ADMIN_PORT})
  --origin-host <host>    Origin-Host identity
  --origin-realm <realm>  Origin-Realm (default: ${DEFAULT_ORIGIN_REALM})

Available stacks: ${AVAILABLE_STACKS:-none}

Examples:
  $(basename "$0")                                    # interactive
  $(basename "$0") -n pcrf-mock -s "gx rx" -r server  # non-interactive
  $(basename "$0") -n pgw-sim -s gx -r client -o ./examples/peers  # custom output dir

EOF
}

# =============================================================================
# Input helpers
# =============================================================================
ask() {
    local prompt="$1" default="$2" var="$3"
    if [ -n "${default}" ]; then
        read -rp "${prompt} [${default}]: " val
        eval "${var}='${val:-${default}}'"
    else
        read -rp "${prompt}: " val
        eval "${var}='${val}'"
    fi
}

# =============================================================================
# Parse CLI args (non-interactive mode)
# =============================================================================
PEER_NAME="" STACKS="" ROLE=""
SERVER_PORT="" PEER_HOST="" PEER_PORT=""
ADMIN_PORT="" ORIGIN_HOST="" ORIGIN_REALM=""
OUTPUT_DIR="" EXTRA_H2AGENT_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -n|--name) PEER_NAME="$2"; shift 2 ;;
        -s|--stacks) STACKS="$2"; shift 2 ;;
        -r|--role) ROLE="$2"; shift 2 ;;
        -o|--output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --server-port) SERVER_PORT="$2"; shift 2 ;;
        --peer-host) PEER_HOST="$2"; shift 2 ;;
        --peer-port) PEER_PORT="$2"; shift 2 ;;
        --admin-port) ADMIN_PORT="$2"; shift 2 ;;
        --origin-host) ORIGIN_HOST="$2"; shift 2 ;;
        --origin-realm) ORIGIN_REALM="$2"; shift 2 ;;
        --extra-h2agent-args) EXTRA_H2AGENT_ARGS="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

PEERS_DIR="${OUTPUT_DIR:-${DEFAULT_OUTPUT_DIR}}"

# =============================================================================
# Interactive mode (fill in missing values)
# =============================================================================
echo
echo "=== h2diagent Peer Generator ==="
echo

if [ -z "${PEER_NAME}" ]; then
    ask "Peer name" "" PEER_NAME
fi

if [ -z "${PEER_NAME}" ]; then
    echo "ERROR: peer name is required"
    exit 1
fi

if [ -z "${STACKS}" ]; then
    echo "Available stacks: ${AVAILABLE_STACKS}"
    ask "Stacks (space-separated)" "gx" STACKS
fi

if [ -z "${ROLE}" ]; then
    ask "Role [server/client/both]" "server" ROLE
fi

# Role-specific questions
case "${ROLE}" in
    server)
        [ -z "${SERVER_PORT}" ] && ask "Diameter server port" "${DEFAULT_DIAMETER_PORT}" SERVER_PORT
        PEER_HOST=""
        PEER_PORT=""
        ;;
    client)
        SERVER_PORT="0"
        [ -z "${PEER_HOST}" ] && ask "Remote peer host" "" PEER_HOST
        [ -z "${PEER_PORT}" ] && ask "Remote peer port" "${DEFAULT_DIAMETER_PORT}" PEER_PORT
        ;;
    both)
        [ -z "${SERVER_PORT}" ] && ask "Diameter server port" "${DEFAULT_DIAMETER_PORT}" SERVER_PORT
        [ -z "${PEER_HOST}" ] && ask "Remote peer host" "" PEER_HOST
        [ -z "${PEER_PORT}" ] && ask "Remote peer port" "${DEFAULT_DIAMETER_PORT}" PEER_PORT
        ;;
    *)
        echo "ERROR: role must be server, client, or both"
        exit 1
        ;;
esac

[ -z "${ADMIN_PORT}" ] && ask "h2agent admin port" "${DEFAULT_ADMIN_PORT}" ADMIN_PORT
[ -z "${ORIGIN_HOST}" ] && ask "Origin-Host" "${PEER_NAME}.${DEFAULT_ORIGIN_REALM}" ORIGIN_HOST
[ -z "${ORIGIN_REALM}" ] && ask "Origin-Realm" "${DEFAULT_ORIGIN_REALM}" ORIGIN_REALM

# =============================================================================
# Derived values
# =============================================================================
PEER_DIR="${PEERS_DIR}/${PEER_NAME}"
# HTTP/2 server port (exposed, for outbound triggers - only if client role)
HTTP2_SERVER_PORT=0
if [ "${ROLE}" = "client" ] || [ "${ROLE}" = "both" ]; then
    HTTP2_SERVER_PORT=$((ADMIN_PORT + 4000))
fi

# =============================================================================
# Generate peer directory
# =============================================================================
if [ -d "${PEER_DIR}" ]; then
    echo ""
    echo "WARNING: ${PEER_DIR} already exists."
    read -rp "Overwrite? [y/N]: " confirm
    [ "${confirm}" != "y" ] && echo "Aborted." && exit 0
    rm -r "${PEER_DIR}"
fi

mkdir -p "${PEER_DIR}/stacks"
mkdir -p "${PEER_DIR}/programming"

echo ""
echo "Generating peer: ${PEER_NAME}"
echo "  Role: ${ROLE}"
echo "  Stacks: ${STACKS}"
echo "  Origin: ${ORIGIN_HOST} / ${ORIGIN_REALM}"

# --- Copy stack dictionaries ---
for stack in ${STACKS}; do
    src="${STACKS_DIR}/${stack}.json"
    if [ -f "${src}" ]; then
        cp "${src}" "${PEER_DIR}/stacks/"
        echo "  Stack: ${stack} (copied)"
    else
        echo "  WARNING: stack '${stack}' not found in ${STACKS_DIR}"
    fi
done

# --- Generate config.env ---
cat > "${PEER_DIR}/config.env" << EOF
# Peer configuration (generated by create-peer.sh)
PEER_NAME=${PEER_NAME}
ROLE=${ROLE}
STACKS="${STACKS}"
ORIGIN_HOST=${ORIGIN_HOST}
ORIGIN_REALM=${ORIGIN_REALM}
DIAMETER_SERVER_PORT=${SERVER_PORT}
DIAMETER_PEER_HOST=${PEER_HOST}
DIAMETER_PEER_PORT=${PEER_PORT}
ADMIN_PORT=${ADMIN_PORT}
H2DIAGENT_IMAGE=${DEFAULT_H2DIAGENT_IMAGE}
H2AGENT_IMAGE=${DEFAULT_H2AGENT_IMAGE}
EOF

# --- Generate docker-compose.yml ---
DICT_VOLUMES=""
DICT_ARGS=""
for stack in ${STACKS}; do
    DICT_VOLUMES="${DICT_VOLUMES}      - ./stacks/${stack}.json:/opt/stacks/${stack}.json:ro
"
    DICT_ARGS="${DICT_ARGS}      - \"--dictionary\"
      - \"/opt/stacks/${stack}.json\"
"
done

# H2agent ports derived from admin port (avoids collisions between peers)
H2AGENT_TRAFFIC_PORT=$((ADMIN_PORT + 1000))
H2AGENT_PROM_PORT=$((ADMIN_PORT + 2000))
H2DIAGENT_PROM_PORT=$((ADMIN_PORT + 3000))

cat > "${PEER_DIR}/docker-compose.yml" << EOF
# Generated by create-peer.sh - peer: ${PEER_NAME} (${ROLE})
# Host networking for simplicity.
version: '3.3'

services:
  h2diagent:
    image: ${DEFAULT_H2DIAGENT_IMAGE}
    container_name: ${PEER_NAME}-h2diagent
    network_mode: host
    volumes:
${DICT_VOLUMES}    command:
      - "--diameter-port"
      - "${SERVER_PORT}"
$([ -n "${PEER_HOST}" ] && echo "      - \"--diameter-peer-host\"
      - \"${PEER_HOST}\"
      - \"--diameter-peer-port\"
      - \"${PEER_PORT}\"")
      - "--origin-host"
      - "${ORIGIN_HOST}"
      - "--origin-realm"
      - "${ORIGIN_REALM}"
      - "--h2agent-host"
      - "localhost"
      - "--h2agent-port"
      - "${H2AGENT_TRAFFIC_PORT}"
      - "--http2-server-port"
      - "${HTTP2_SERVER_PORT}"
${DICT_ARGS}      - "--prometheus-port"
      - "${H2DIAGENT_PROM_PORT}"
      - "--verbose"
    depends_on:
      - h2agent

  h2agent:
    image: ${DEFAULT_H2AGENT_IMAGE}
    container_name: ${PEER_NAME}-h2agent
    network_mode: host
    command:
      - "--traffic-server-port"
      - "${H2AGENT_TRAFFIC_PORT}"
      - "--admin-port"
      - "${ADMIN_PORT}"
      - "--prometheus-port"
      - "${H2AGENT_PROM_PORT}"
      - "--verbose"
$(for arg in ${EXTRA_H2AGENT_ARGS}; do echo "      - \"${arg}\""; done)
EOF

# --- Generate run.bash ---
cat > "${PEER_DIR}/run.bash" << 'RUNEOF'
#!/bin/bash
# Start peer (sourceable or executable)
PEER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
source "${PEER_DIR}/config.env"

echo "Starting peer: ${PEER_NAME} (${ROLE})"
cd "${PEER_DIR}" && docker-compose up -d

# Wait for h2agent readiness
echo -n "Waiting for h2agent..."
for i in $(seq 1 30); do
    curl -sf --http2-prior-knowledge "http://localhost:${ADMIN_PORT}/admin/v1/health" >/dev/null 2>&1 && break
    sleep 0.5
    echo -n "."
done
echo ""

if curl -sf --http2-prior-knowledge "http://localhost:${ADMIN_PORT}/admin/v1/health" >/dev/null 2>&1; then
    echo "Peer ${PEER_NAME} is ready (admin: http://localhost:${ADMIN_PORT})"
else
    echo "ERROR: peer ${PEER_NAME} failed to start"
    return 1 2>/dev/null || exit 1
fi

# Load programming if present
if ls "${PEER_DIR}"/programming/*.json >/dev/null 2>&1; then
    echo "Loading programming..."
    ADMIN="http://localhost:${ADMIN_PORT}"
    for f in "${PEER_DIR}"/programming/server-matching*.json; do
        [ -f "$f" ] && curl -sf --http2-prior-knowledge -XPOST "${ADMIN}/admin/v1/server-matching" -H "content-type:application/json" -d @"$f" >/dev/null && echo "  $(basename $f)"
    done
    for f in "${PEER_DIR}"/programming/server-provision*.json; do
        [ -f "$f" ] && curl -sf --http2-prior-knowledge -XPOST "${ADMIN}/admin/v1/server-provision" -H "content-type:application/json" -d @"$f" >/dev/null && echo "  $(basename $f)"
    done
    for f in "${PEER_DIR}"/programming/client-endpoint*.json; do
        [ -f "$f" ] && curl -sf --http2-prior-knowledge -XPOST "${ADMIN}/admin/v1/client-endpoint" -H "content-type:application/json" -d @"$f" >/dev/null && echo "  $(basename $f)"
    done
    for f in "${PEER_DIR}"/programming/client-provision*.json; do
        [ -f "$f" ] && curl -sf --http2-prior-knowledge -XPOST "${ADMIN}/admin/v1/client-provision" -H "content-type:application/json" -d @"$f" >/dev/null && echo "  $(basename $f)"
    done
fi

echo "Peer ${PEER_NAME} running."
cd - &>/dev/null
RUNEOF
chmod +x "${PEER_DIR}/run.bash"

# --- Generate stop.bash ---
cat > "${PEER_DIR}/stop.bash" << 'STOPEOF'
#!/bin/bash
# Stop peer (sourceable or executable)
PEER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
source "${PEER_DIR}/config.env"
echo "Stopping peer: ${PEER_NAME}"
cd "${PEER_DIR}" && docker-compose down
cd - &>/dev/null
STOPEOF
chmod +x "${PEER_DIR}/stop.bash"

# --- Generate ps.bash ---
cat > "${PEER_DIR}/ps.bash" << 'PSEOF'
#!/bin/bash
# Show peer containers status (sourceable or executable)
PEER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
source "${PEER_DIR}/config.env"
echo "Peer: ${PEER_NAME} (${ROLE})"
echo "  Admin:    http://localhost:${ADMIN_PORT}"
[ "${DIAMETER_SERVER_PORT}" != "0" ] && echo "  Diameter:  localhost:${DIAMETER_SERVER_PORT}"
[ -n "${DIAMETER_PEER_HOST}" ] && echo "  Peer:      ${DIAMETER_PEER_HOST}:${DIAMETER_PEER_PORT}"
echo ""
cd "${PEER_DIR}" && docker-compose ps
cd - &>/dev/null
PSEOF
chmod +x "${PEER_DIR}/ps.bash"

# --- Generate placeholder programming ---
if [ "${ROLE}" = "server" ] || [ "${ROLE}" = "both" ]; then
    cat > "${PEER_DIR}/programming/server-matching.json" << EOF
{"algorithm": "FullMatching"}
EOF

    # Generate default provisions based on loaded stacks
    PROVISIONS="["
    FIRST=true
    for stack in ${STACKS}; do
        case "${stack}" in
            gx)
                [ "${FIRST}" = "true" ] || PROVISIONS="${PROVISIONS},"
                PROVISIONS="${PROVISIONS}
  {
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/gx/CCR\",
    \"responseCode\":200,
    \"responseHeaders\":{\"content-type\":\"application/json\"},
    \"responseBody\":{
      \"Session-Id\":\"\",\"Result-Code\":2001,
      \"Origin-Host\":\"${ORIGIN_HOST}\",\"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777238,\"CC-Request-Type\":0,\"CC-Request-Number\":0
    },
    \"transform\":[
      {\"source\":\"request.body./Session-Id\",\"target\":\"response.body.json.string./Session-Id\"},
      {\"source\":\"request.body./CC-Request-Type\",\"target\":\"response.body.json.integer./CC-Request-Type\"},
      {\"source\":\"request.body./CC-Request-Number\",\"target\":\"response.body.json.unsigned./CC-Request-Number\"}
    ]
  }"
                FIRST=false
                ;;
            rx)
                [ "${FIRST}" = "true" ] || PROVISIONS="${PROVISIONS},"
                PROVISIONS="${PROVISIONS}
  {
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/rx/AAR\",
    \"responseCode\":200,
    \"responseHeaders\":{\"content-type\":\"application/json\"},
    \"responseBody\":{
      \"Session-Id\":\"\",\"Result-Code\":2001,
      \"Origin-Host\":\"${ORIGIN_HOST}\",\"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777236
    },
    \"transform\":[
      {\"source\":\"request.body./Session-Id\",\"target\":\"response.body.json.string./Session-Id\"}
    ]
  }"
                FIRST=false
                ;;
            sy)
                [ "${FIRST}" = "true" ] || PROVISIONS="${PROVISIONS},"
                PROVISIONS="${PROVISIONS}
  {
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/sy/SNR\",
    \"responseCode\":200,
    \"responseHeaders\":{\"content-type\":\"application/json\"},
    \"responseBody\":{
      \"Session-Id\":\"\",\"Result-Code\":2001,
      \"Origin-Host\":\"${ORIGIN_HOST}\",\"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777302
    },
    \"transform\":[
      {\"source\":\"request.body./Session-Id\",\"target\":\"response.body.json.string./Session-Id\"}
    ]
  }"
                FIRST=false
                ;;
        esac
    done
    PROVISIONS="${PROVISIONS}
]"
    echo "${PROVISIONS}" > "${PEER_DIR}/programming/server-provision.json"
    echo "  Created: programming/server-provision.json (default provisions for: ${STACKS})"
fi

if [ "${ROLE}" = "client" ] || [ "${ROLE}" = "both" ]; then
    # Client endpoint: points to the local h2diagent HTTP/2 server
    cat > "${PEER_DIR}/programming/client-endpoint.json" << EOF
{
  "id": "diameter-gw",
  "host": "localhost",
  "port": ${HTTP2_SERVER_PORT},
  "secure": false
}
EOF

    # Generate default client provisions based on loaded stacks
    CPROVISIONS="["
    FIRST=true
    for stack in ${STACKS}; do
        case "${stack}" in
            gx)
                [ "${FIRST}" = "true" ] || CPROVISIONS="${CPROVISIONS},"
                CPROVISIONS="${CPROVISIONS}
  {
    \"id\":\"send-gx-ccr\",
    \"endpoint\":\"diameter-gw\",
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/gx/CCR\",
    \"requestHeaders\":{\"content-type\":\"application/json\"},
    \"requestBody\":{
      \"Session-Id\":\"${ORIGIN_HOST};session-001;1\",
      \"Origin-Host\":\"${ORIGIN_HOST}\",
      \"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Destination-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777238,
      \"CC-Request-Type\":1,
      \"CC-Request-Number\":0
    },
    \"expectedResponseStatusCode\":200
  }"
                FIRST=false
                ;;
            rx)
                [ "${FIRST}" = "true" ] || CPROVISIONS="${CPROVISIONS},"
                CPROVISIONS="${CPROVISIONS}
  {
    \"id\":\"send-rx-aar\",
    \"endpoint\":\"diameter-gw\",
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/rx/AAR\",
    \"requestHeaders\":{\"content-type\":\"application/json\"},
    \"requestBody\":{
      \"Session-Id\":\"${ORIGIN_HOST};session-001;1\",
      \"Origin-Host\":\"${ORIGIN_HOST}\",
      \"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Destination-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777236
    },
    \"expectedResponseStatusCode\":200
  }"
                FIRST=false
                ;;
            sy)
                [ "${FIRST}" = "true" ] || CPROVISIONS="${CPROVISIONS},"
                CPROVISIONS="${CPROVISIONS}
  {
    \"id\":\"send-sy-snr\",
    \"endpoint\":\"diameter-gw\",
    \"requestMethod\":\"POST\",
    \"requestUri\":\"/diameter/sy/SNR\",
    \"requestHeaders\":{\"content-type\":\"application/json\"},
    \"requestBody\":{
      \"Session-Id\":\"${ORIGIN_HOST};session-001;1\",
      \"Origin-Host\":\"${ORIGIN_HOST}\",
      \"Origin-Realm\":\"${ORIGIN_REALM}\",
      \"Destination-Realm\":\"${ORIGIN_REALM}\",
      \"Auth-Application-Id\":16777302
    },
    \"expectedResponseStatusCode\":200
  }"
                FIRST=false
                ;;
        esac
    done
    CPROVISIONS="${CPROVISIONS}
]"
    echo "${CPROVISIONS}" > "${PEER_DIR}/programming/client-provision.json"
    echo "  Created: programming/client-endpoint.json + client-provision.json (triggers: ${STACKS})"
fi

# --- Summary ---
echo ""
echo "====================================="
echo "Peer created: ${PEER_DIR}"
echo "====================================="
echo ""
echo "Files:"
find "${PEER_DIR}" -type f | sort | sed "s|${PEER_DIR}/|  |"
echo ""
echo "Usage:"
echo "  source ${PEER_DIR}/run.bash    # start"
echo "  source ${PEER_DIR}/stop.bash   # stop"
echo ""
echo "h2agent admin: http://localhost:${ADMIN_PORT}"
[ "${SERVER_PORT}" != "0" ] && echo "Diameter server: localhost:${SERVER_PORT}"
[ -n "${PEER_HOST}" ] && echo "Diameter client -> ${PEER_HOST}:${PEER_PORT}"
echo ""
