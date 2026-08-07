#!/bin/bash
###############################################################################
# h2diagent component test procedure
#
# Deploys the ct-h2diagent chart (two Diameter peers: server + client, each an
# h2diagent + h2agent sidecar pod) plus a pytest runner pod on the current
# kubernetes context (minikube), then executes the test suite inside that pod.
###############################################################################

#############
# VARIABLES #
#############
project_root_dir="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"

CHART_NAME=ct-h2diagent
NAMESPACE="ns-${CHART_NAME}"
HELM_CHART="helm/${CHART_NAME}"
H2DIAGENT_CHART="helm/h2diagent"

TAG=${TAG:-latest}          # h2diagent image tag
CT_TAG=${CT_TAG:-latest}    # ct-h2diagent (pytest) image tag
H2AGENT_TAG=${H2AGENT_TAG:-latest}  # h2agent sidecar image tag

# Diameter stacks whose dictionaries are bundled into the chart.
STACKS_TO_BUNDLE=${STACKS_TO_BUNDLE:-"gx"}

# h2agent helper library bundled into the sidecars (troubleshooting on exec).
# Override with an explicit path; default resolves a sibling h2agent checkout.
H2AGENT_HELPERS_BASH=${H2AGENT_HELPERS_BASH:-"${project_root_dir}/../testillano_h2agent.master/tools/helpers.bash"}

# shellcheck disable=SC2207,SC2012
ALL_TESTS=( $(ls "${project_root_dir}"/ct/src/*/*_test.py 2>/dev/null | awk -F/ '{ print $(NF-1)"/"$NF }') )

s_XTRA_HELM_SETS="-"
[ -n "${XTRA_HELM_SETS}" ] && s_XTRA_HELM_SETS=${XTRA_HELM_SETS}
s_SKIP_HELM_DEPS=false
[ -n "${SKIP_HELM_DEPS}" ] && s_SKIP_HELM_DEPS=true

#############
# FUNCTIONS #
#############
usage() {
  cat << EOF
Usage: $0 [-h|--help] [action: [all]|deploy|test|hints] [ pytest extra options ]

Positional options:
  -h|--help: this help
  action:    Deploy, tests and hints shown by default, but also just 'deploy',
             'test' or 'hints' could be requested.
  pytest options: extra options passed to the 'pytest' executable.

Prepend variables:
  XTRA_HELM_SETS:  additional setters for helm install execution.
  SKIP_HELM_DEPS:  non-empty value skips helm dependencies update.
  TAG:             h2diagent image tag for deployment (latest by default).
  CT_TAG:          ct-h2diagent image tag for deployment (latest by default).
  H2AGENT_TAG:     h2agent sidecar image tag (latest by default).
  STACKS_TO_BUNDLE: space-separated Diameter stacks to bundle (default: gx).
  H2AGENT_HELPERS_BASH: path to h2agent tools/helpers.bash to bundle into the
                   sidecars (default: sibling ../testillano_h2agent.master).

Examples:
  TAG=test1 CT_TAG=test1 $0
  $0 deploy
  $0 hints
  $0 test gx_functional/gxf_ccri_test.py
EOF
}

# $1: namespace; $2: optional prefix app filter
get_pod() {
  local filter=
  [ -n "$2" ] && filter+=" -l app.kubernetes.io/name=${2}"
  # shellcheck disable=SC2086
  kubectl --namespace "$1" get pod --no-headers ${filter} | awk '{ if ($3 == "Running") print $1 }'
  return $?
}

# $1: test pod; $2-@: pytest arguments
do_test() {
  local test_pod=$1
  shift
  # ubuntu's /bin/sh is dash (no 'source'); use bash -c.
  # shellcheck disable=SC2068
  kubectl exec -i "${test_pod}" -c test -n "${NAMESPACE}" -- bash -c "source /venv/bin/activate && pytest $@"
}

# Copy the required stack dictionaries into the h2diagent chart so the chart's
# ConfigMap (Files.Glob dictionaries/*.json) can bundle them.
bundle_dictionaries() {
  local dst="${project_root_dir}/${H2DIAGENT_CHART}/dictionaries"
  mkdir -p "${dst}"
  local copied=0
  for stack in ${STACKS_TO_BUNDLE}; do
    local src="${project_root_dir}/tools/stacks/${stack}.json"
    if [ -f "${src}" ]; then
      cp "${src}" "${dst}/${stack}.json"
      echo "  bundled dictionary: ${stack}.json"
      copied=$((copied + 1))
    else
      echo "  WARNING: stack '${stack}' not found at ${src}"
    fi
  done
  [ ${copied} -eq 0 ] && echo "ERROR: no dictionaries bundled" && return 1
  return 0
}

# Copy the h2agent helper library into the h2diagent chart so the sidecars can
# mount and source it (parity with the default h2agent chart). Best-effort: if
# the source is missing, the sidecars still load helpers.bash with the port
# variables only.
bundle_helpers() {
  local dst="${project_root_dir}/${H2DIAGENT_CHART}/helpers"
  mkdir -p "${dst}"
  if [ -f "${H2AGENT_HELPERS_BASH}" ]; then
    cp "${H2AGENT_HELPERS_BASH}" "${dst}/native-helpers.bash"
    echo "  bundled h2agent helpers -> native-helpers.bash (from ${H2AGENT_HELPERS_BASH})"
  else
    rm -f "${dst}/native-helpers.bash"
    echo "  NOTE: h2agent helpers not found at ${H2AGENT_HELPERS_BASH}"
    echo "        sidecars will load helpers.bash with port variables only."
    echo "        Set H2AGENT_HELPERS_BASH to bundle the full function library."
  fi
}

#############
# EXECUTION #
#############
# shellcheck disable=SC2164
cd "${project_root_dir}"

# shellcheck disable=SC2166
[ "$1" = "-h" -o "$1" = "--help" ] && usage && exit 0

action=$1
s_action=${action:-"Deploy and test"}
DEPLOY=true
TEST=true
if [ -n "$1" ]; then
  case ${action} in
    all)    s_action="Deploy and test" ;;
    deploy) TEST= ;;
    test)   DEPLOY= ;;
    hints)  DEPLOY= ; TEST= ;;
    *) echo "ERROR: invalid action (allowed: all, deploy, test, hints)" && exit 1 ;;
  esac
fi

echo
echo "==============================="
echo "Component test procedure script"
echo "==============================="
echo
echo "(-h|--help for more information)"
echo
echo "Chart name:      ${CHART_NAME}"
echo "Namespace:       ${NAMESPACE}"
echo "TAG:             ${TAG}"
echo "CT_TAG:          ${CT_TAG}"
echo "H2AGENT_TAG:     ${H2AGENT_TAG}"
echo "STACKS_TO_BUNDLE: ${STACKS_TO_BUNDLE}"
echo "XTRA_HELM_SETS:  ${s_XTRA_HELM_SETS}"
echo "SKIP_HELM_DEPS:  ${s_SKIP_HELM_DEPS}"
shift
[ $# -gt 0 -a -n "${TEST}" ] && echo "Pytest arguments: $*"
echo

RC=0
if [ -n "${DEPLOY}" ]; then
  echo -e "\nCleaning up ..."
  helm delete "${CHART_NAME}" -n "${NAMESPACE}" &>/dev/null
  kubectl delete namespace "${NAMESPACE}" &>/dev/null

  echo -e "\nBundling Diameter dictionaries ..."
  bundle_dictionaries || exit 1

  echo -e "\nBundling h2agent helpers ..."
  bundle_helpers

  echo -e "\nUpdating helm chart dependencies ..."
  if [ -n "${SKIP_HELM_DEPS}" ]; then
    echo "Skipped !"
  else
    helm dep update "${HELM_CHART}" &>/dev/null || { echo "Error !"; exit 1 ; }
  fi

  echo -e "\nDeploying chart ..."
  kubectl create namespace "${NAMESPACE}" &>/dev/null
  # shellcheck disable=SC2086
  helm install "${CHART_NAME}" "${HELM_CHART}" -n "${NAMESPACE}" --wait --timeout 300s \
    --set server.image.tag="${TAG}" \
    --set client.image.tag="${TAG}" \
    --set server.h2agent.image.tag="${H2AGENT_TAG}" \
    --set client.h2agent.image.tag="${H2AGENT_TAG}" \
    --set test.image.tag="${CT_TAG}" \
    ${XTRA_HELM_SETS} || { echo "Error !"; exit 1 ; }
else
  echo -e "\nDeployment skipped !"
fi

if [ -n "${TEST}" ]; then
  echo -e "\nExecuting tests ..."
  test_pod="$(get_pod "${NAMESPACE}" ct-h2diagent)"
  [ -z "${test_pod}" ] && test_pod="$(get_pod "${NAMESPACE}" test)"
  [ -z "${test_pod}" ] && echo "Missing target pod for test" && exit 1
  # shellcheck disable=SC2068
  do_test "${test_pod}" $@
  RC=$?
else
  echo -e "\nTests skipped !"
fi

# Final hints:
deployed=$(helm list -q --deployed -n "${NAMESPACE}" | grep -w "${CHART_NAME}")
if [ ${RC} -eq 0 ] && [ -n "${deployed}" ]; then
  cat << EOF

You may inspect the peers via their h2agent admin APIs by port-forwarding:

  server_pod=\$(kubectl get pod -n ${NAMESPACE} -l app.kubernetes.io/name=server -o name | head -1)
  client_pod=\$(kubectl get pod -n ${NAMESPACE} -l app.kubernetes.io/name=client -o name | head -1)
  kubectl port-forward -n ${NAMESPACE} \${server_pod} 8074:8074 &   # server h2agent admin
  kubectl port-forward -n ${NAMESPACE} \${client_pod} 8075:8074 &   # client h2agent admin

Then, e.g.:
  curl -s --http2-prior-knowledge http://localhost:8074/admin/v1/server-data | jq .
  curl -s --http2-prior-knowledge http://localhost:8075/admin/v1/client-data | jq .
EOF
fi

echo
echo "[RC=${RC}]"
echo
exit ${RC}
