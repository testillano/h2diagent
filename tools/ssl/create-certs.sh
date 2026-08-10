#!/bin/bash
###############################################################################
# Generate test certificates for secure Diameter (TLS/TCP) with h2diagent.
#
# Produces (PEM) in the current/target directory:
#   ca.crt  ca.key          -- test Certificate Authority
#   server.crt server.key   -- Diameter server (inbound) certificate + key
#   client.crt client.key   -- Diameter client (outbound) certificate + key (mTLS)
#
# The server and client certificates are signed by the same CA, so:
#   - the client can verify the server with  --diameter-client-ca ca.crt
#   - the peer (or another h2diagent server configured for mTLS) can verify the
#     client with ca.crt.
#
# Usage:
#   ./create-certs.sh [CN] [key-password]
#     CN            Common Name / hostname for server & client (default: localhost)
#     key-password  If provided, server/client private keys are encrypted with it
#                   (pass it to h2diagent via --diameter-server-key-password /
#                   --diameter-client-key-password).
#
# NOTE: TLS applies to the TCP transport only. Diameter over SCTP would use
# DTLS/SCTP (RFC 6083), which is NOT supported by h2diagent (see README gap).
###############################################################################
set -euo pipefail

SCR_DIR=$(dirname "$(readlink -f "$0")")
CN="${1:-localhost}"
PASS="${2:-}"
DAYS=3650
KEY_BITS=2048
SUBJ="/C=ES/ST=Madrid/L=Madrid/O=testillano/OU=h2diagent/CN=${CN}"
SUBJ_CA="/C=ES/ST=Madrid/L=Madrid/O=testillano/OU=h2diagent-CA/CN=h2diagent-Test-CA"

cd "${SCR_DIR}"

gen_key() {  # $1 = output key file
  if [ -n "${PASS}" ]; then
    openssl genrsa -aes256 -passout pass:"${PASS}" -out "$1" "${KEY_BITS}"
  else
    openssl genrsa -out "$1" "${KEY_BITS}"
  fi
}

passin_args() {  # echoes '-passin pass:...' when a password is set (word-split on use)
  [ -n "${PASS}" ] && printf -- '-passin pass:%s' "${PASS}" || true
}

# --- CA (unencrypted key for simplicity) ---
openssl genrsa -out ca.key "${KEY_BITS}"
openssl req -x509 -new -nodes -key ca.key -sha256 -days "${DAYS}" -subj "${SUBJ_CA}" -out ca.crt

# --- Server certificate (signed by CA) ---
gen_key server.key
# shellcheck disable=SC2046
openssl req -new -key server.key $(passin_args) -subj "${SUBJ}" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -days "${DAYS}" -sha256 -out server.crt

# --- Client certificate (signed by CA; for mutual TLS) ---
gen_key client.key
# shellcheck disable=SC2046
openssl req -new -key client.key $(passin_args) -subj "${SUBJ}" -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -days "${DAYS}" -sha256 -out client.crt

rm -f server.csr client.csr

echo
echo "Generated in ${SCR_DIR}:"
echo "  ca.crt ca.key  server.crt server.key  client.crt client.key"
echo "  CN=${CN}${PASS:+  (private keys encrypted with the provided password)}"
echo
echo "Secure server (inbound):"
echo "  h2diagent --diameter-server-crt ${SCR_DIR}/server.crt --diameter-server-key ${SCR_DIR}/server.key${PASS:+ --diameter-server-key-password '<pass>'} ..."
echo
echo "Secure client (outbound), verifying the peer with the CA:"
echo "  h2diagent --secure-diameter-client --diameter-client-ca ${SCR_DIR}/ca.crt ..."
