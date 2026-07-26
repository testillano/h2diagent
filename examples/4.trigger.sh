#!/bin/bash
# 4.trigger.sh - Fire the configured trigger on the current deployment
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="${SCRIPT_DIR}/deployment"

[ ! -f "${DEPLOY_DIR}/config.bash" ] && echo "ERROR: no deployment. Run ./1.deploy.sh <example> first." && exit 1

source "${DEPLOY_DIR}/config.bash"

echo "Trigger: ${TRIGGER}"
curl -sf --http2-prior-knowledge "${TRIGGER}"
echo ""
