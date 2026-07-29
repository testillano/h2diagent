#!/bin/bash
# 3.status.sh - Show status of the current deployment
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="${SCRIPT_DIR}/deployment"

[ ! -f "${DEPLOY_DIR}/config.sh" ] && echo "ERROR: no deployment. Run ./1.deploy.sh <example> first." && exit 1

source "${DEPLOY_DIR}/config.sh"

for peer_def in "${PEERS[@]}"; do
    NAME=$(echo "${peer_def}" | awk '{print $1}')
    [ -f "${DEPLOY_DIR}/${NAME}/ps.sh" ] && source "${DEPLOY_DIR}/${NAME}/ps.sh"
    echo ""
done
