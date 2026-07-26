#!/bin/bash
# =============================================================================
# h2diagent build script (flat multi-stage model)
# =============================================================================
set -e

SCR="$(readlink -f "$0")"
SCR_DIR="$(dirname "${SCR}")"
cd "${SCR_DIR}"

DOCKERFILE=Dockerfile
registry=ghcr.io/testillano

parse_arg() {
  grep "^ARG ${1}=" "${DOCKERFILE}" | head -1 | cut -d= -f2
}

make_procs__dflt=$(grep processor /proc/cpuinfo -c)
build_type__dflt=$(parse_arg build_type)
image_tag__dflt=latest

usage() {
  cat << EOF

  Usage: $0 [--builder|--ut|--image]

         (no args):   builds everything (--image).
         --builder:   builds deps stage only (builder image).
         --ut:        builds unit-test image.
         --image:     builds runtime image (production).

         Environment variables (override any Dockerfile ARG):
           image_tag, make_procs, build_type, boost_ver, ert_logger_ver,
           nlohmann_json_ver, ert_metrics_ver, ert_http2comm_ver,
           ert_diametercodec_ver, ert_diametercomm_ver, google_test_ver

         Other: DBUILD_XTRA_OPTS (extra docker build options)

EOF
}

resolve() {
  local var=$1
  local val="${!var}"
  [ -z "${val}" ] && val="$(eval echo \$${var}__dflt)"
  echo "${val}"
}

build_args() {
  local bargs=""
  bargs+=" --build-arg make_procs=$(resolve make_procs)"
  bargs+=" --build-arg build_type=$(resolve build_type)"
  # Pass any explicitly set version overrides
  for v in boost_ver ert_logger_ver nlohmann_json_ver pboettch_jsonschemavalidator_ver \
           jupp0r_prometheuscpp_ver civetweb_civetweb_ver ert_metrics_ver \
           testillano_nghttp2_ver ert_http2comm_ver \
           ert_diametercodec_ver ert_diametercomm_ver google_test_ver; do
    [ -n "${!v}" ] && bargs+=" --build-arg ${v}=${!v}"
  done
  echo "${bargs}"
}

build_builder() {
  echo "=== Build h2diagent_builder (deps stage) ==="
  local tag=$(resolve image_tag)
  docker build --target deps \
    -t ${registry}/h2diagent_builder:${tag} \
    $(build_args) ${DBUILD_XTRA_OPTS} .
  echo "Built: ${registry}/h2diagent_builder:${tag}"
}

build_ut() {
  echo "=== Build h2diagent_ut (unit-test image) ==="
  local tag=$(resolve image_tag)
  docker build --target unit-test \
    -t ${registry}/h2diagent_ut:${tag} \
    $(build_args) ${DBUILD_XTRA_OPTS} .
  echo "Built: ${registry}/h2diagent_ut:${tag}"
}

build_image() {
  echo "=== Build h2diagent (runtime image) ==="
  local tag=$(resolve image_tag)
  docker build --target runtime \
    -t ${registry}/h2diagent:${tag} \
    $(build_args) ${DBUILD_XTRA_OPTS} .
  echo "Built: ${registry}/h2diagent:${tag}"
}

case "$1" in
  --builder) build_builder ;;
  --ut)      build_ut ;;
  --image)   build_image ;;
  -h|--help) usage; exit 0 ;;
  "")        build_image ;;
  *)         echo "Unknown: $1"; usage; exit 1 ;;
esac
