#!/bin/bash
# 2.run.sh - Start the current deployment
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="${SCRIPT_DIR}/deployment"

[ ! -f "${DEPLOY_DIR}/config.sh" ] && echo "ERROR: no deployment. Run ./1.deploy.sh <example> first." && exit 1

source "${DEPLOY_DIR}/config.sh"

echo "Starting..."
for peer_def in "${PEERS[@]}"; do
    NAME=$(echo "${peer_def}" | awk '{print $1}')
    source "${DEPLOY_DIR}/${NAME}/run.sh"
done

echo "Waiting for Diameter peering (${PEERING_WAIT:-4}s)..."
sleep ${PEERING_WAIT:-4}
echo "Ready."
