#!/bin/bash
# =============================================================================
# 1.deploy.sh <example> - Deploy peers from a .example configuration
# =============================================================================
# Creates a ./deployment/ directory with all peers. Replaces any previous deployment.
#
# Usage: ./1.deploy.sh <example>
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EXAMPLE="${1:-}"

# Accept both "<name>" and "<name>.example".
if [ -n "${EXAMPLE}" ] && [ ! -f "${SCRIPT_DIR}/${EXAMPLE}" ] && [ -f "${SCRIPT_DIR}/${EXAMPLE}.example" ]; then
    EXAMPLE="${EXAMPLE}.example"
fi

if [ -z "${EXAMPLE}" ] || [ ! -f "${SCRIPT_DIR}/${EXAMPLE}" ]; then
    echo "Usage: $(basename $0) <example file>"
    echo ""
    echo "Deploys the given .example configuration into ./deployment/"
    echo "Any previous deployment is removed."
    echo ""
    echo "Available:"
    for f in "${SCRIPT_DIR}"/*.example; do
        name=$(basename ${f} .example)
        desc=$(head -1 "${f}" | sed 's/^# *//')
        echo "  ${name}: ${desc}"
    done
    exit 1
fi

source "${SCRIPT_DIR}/${EXAMPLE}"
echo

DEPLOY_DIR="${SCRIPT_DIR}/deployment"
rm -r "${DEPLOY_DIR}" 2>/dev/null || true
mkdir -p "${DEPLOY_DIR}"

# Save which example is deployed (for other scripts)
cp "${SCRIPT_DIR}/${EXAMPLE}" "${DEPLOY_DIR}/config.bash"

echo "Deploying: $(basename ${EXAMPLE} .example)"

# Optional Diameter TLS: generate a throwaway CA + server/client certs once,
# mounted into every peer's h2diagent container at /ssl. Enabled by 'SSL=1' in
# the .example (the TLS args in EXTRA_H2DIAGENT_ARGS reference /ssl/... paths).
SSL_ARG=""
if [ "${SSL:-0}" = "1" ]; then
    mkdir -p "${DEPLOY_DIR}/ssl"
    cp "${PROJECT_DIR}/tools/ssl/create-certs.sh" "${DEPLOY_DIR}/ssl/"
    ( cd "${DEPLOY_DIR}/ssl" && ./create-certs.sh localhost >/dev/null 2>&1 )
    SSL_ARG="--ssl-dir ${DEPLOY_DIR}/ssl"
    echo "TLS enabled: certificates generated in ${DEPLOY_DIR}/ssl"
fi

for peer_def in "${PEERS[@]}"; do
    read -r NAME ROLE STACKS REST <<< "${peer_def}"
    ARGS="-n ${NAME} -s \"${STACKS}\" -r ${ROLE} -o ${DEPLOY_DIR}"
    [ -n "${REST}" ] && ARGS="${ARGS} ${REST}"
    [ -n "${EXTRA_H2AGENT_ARGS:-}" ] && ARGS="${ARGS} --extra-h2agent-args \"${EXTRA_H2AGENT_ARGS}\""
    [ -n "${EXTRA_H2DIAGENT_ARGS:-}" ] && ARGS="${ARGS} --extra-h2diagent-args \"${EXTRA_H2DIAGENT_ARGS}\""
    [ "${LOG_LEVEL:-Warning}" != "Warning" ] && ARGS="${ARGS} --log-level ${LOG_LEVEL}"
    [ -n "${SSL_ARG}" ] && ARGS="${ARGS} ${SSL_ARG}"
    eval "${PROJECT_DIR}/tools/create-peer.sh ${ARGS}" 2>&1 | grep -E "Peer created|Created"
done
echo ""
echo "Deployed. Use: ./2.run.sh, ./3.status.sh, ./4.trigger.sh, ./5.stop.sh"
