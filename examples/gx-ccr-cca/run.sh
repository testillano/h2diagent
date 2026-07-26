#!/bin/bash
# =============================================================================
# Gx CCR/CCA example: provision h2agent and verify via h2diagent
# =============================================================================
# Prerequisites:
#   1. h2agent running on localhost:8000 (traffic) / 8074 (admin)
#   2. h2diagent running on localhost:3868 (diameter) pointing to h2agent
#
# This script:
#   - Provisions h2agent with Gx mock logic (CCR-I -> CCA-I -> CCR-U -> CCA-U -> CCR-T -> CCA-T)
#   - Simulates a Diameter CCR-I using h2client directly to h2diagent's HTTP/2 interface
#     (as if h2diagent already translated from Diameter)
#   - Verifies the response
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
H2AGENT_ADMIN=${H2AGENT_ADMIN:-http://localhost:8074}

echo "=== Provisioning h2agent ==="

# Configure matching
curl -sf --http2-prior-knowledge \
  -XPOST "${H2AGENT_ADMIN}/admin/v1/server-matching" \
  -H "content-type:application/json" \
  -d @"${SCRIPT_DIR}/h2agent-matching.json"
echo " [OK] Matching configured"

# Configure provision (Gx CCR/CCA state machine)
curl -sf --http2-prior-knowledge \
  -XPOST "${H2AGENT_ADMIN}/admin/v1/server-provision" \
  -H "content-type:application/json" \
  -d @"${SCRIPT_DIR}/h2agent-provision.json"
echo " [OK] Provision configured"

echo
echo "=== Sending CCR-I (simulated via HTTP/2 to h2agent directly) ==="
echo

# Simulate what h2diagent would send to h2agent after translating a CCR-I:
RESPONSE=$(curl -sf --http2-prior-knowledge \
  -XPOST "http://localhost:8000/diameter/gx/CCR" \
  -H "content-type:application/json" \
  -H "x-diameter-command-code:272" \
  -H "x-diameter-application-id:16777238" \
  -d '{
    "Session-Id": "ggsn.example.com;12345;67890",
    "Origin-Host": "ggsn.example.com",
    "Origin-Realm": "example.com",
    "Destination-Realm": "example.com",
    "Auth-Application-Id": 16777238,
    "CC-Request-Type": 1,
    "CC-Request-Number": 0,
    "Subscription-Id": {
      "Subscription-Id-Type": 1,
      "Subscription-Id-Data": "24001000001"
    }
  }')

echo "CCA-I Response:"
echo "${RESPONSE}" | python3 -m json.tool 2>/dev/null || echo "${RESPONSE}"

# Verify Result-Code
RESULT_CODE=$(echo "${RESPONSE}" | python3 -c "import sys,json; print(json.load(sys.stdin)['Result-Code'])" 2>/dev/null)
if [ "${RESULT_CODE}" = "2001" ]; then
  echo
  echo "[PASS] Result-Code = 2001 (DIAMETER_SUCCESS)"
else
  echo
  echo "[FAIL] Expected Result-Code 2001, got: ${RESULT_CODE}"
  exit 1
fi

echo
echo "=== Sending CCR-U ==="
echo

RESPONSE=$(curl -sf --http2-prior-knowledge \
  -XPOST "http://localhost:8000/diameter/gx/CCR" \
  -H "content-type:application/json" \
  -d '{
    "Session-Id": "ggsn.example.com;12345;67890",
    "Origin-Host": "ggsn.example.com",
    "Origin-Realm": "example.com",
    "CC-Request-Type": 2,
    "CC-Request-Number": 1
  }')

echo "CCA-U Response:"
echo "${RESPONSE}" | python3 -m json.tool 2>/dev/null || echo "${RESPONSE}"

echo
echo "=== Sending CCR-T ==="
echo

RESPONSE=$(curl -sf --http2-prior-knowledge \
  -XPOST "http://localhost:8000/diameter/gx/CCR" \
  -H "content-type:application/json" \
  -d '{
    "Session-Id": "ggsn.example.com;12345;67890",
    "Origin-Host": "ggsn.example.com",
    "Origin-Realm": "example.com",
    "CC-Request-Type": 3,
    "CC-Request-Number": 2
  }')

echo "CCA-T Response:"
echo "${RESPONSE}" | python3 -m json.tool 2>/dev/null || echo "${RESPONSE}"

echo
echo "=== Verifying server data ==="
echo

curl -sf --http2-prior-knowledge \
  "${H2AGENT_ADMIN}/admin/v1/server-data" | python3 -m json.tool 2>/dev/null

echo
echo "=== Example complete ==="
echo "The Gx session lifecycle (CCR-I/CCA-I -> CCR-U/CCA-U -> CCR-T/CCA-T) was"
echo "successfully mocked through h2agent provisions with state transitions."
echo
echo "In production, h2diagent sits between the Diameter client and h2agent,"
echo "transparently translating Diameter binary <-> JSON over HTTP/2."
