# Gx functional CT: a single CCR-I / CCA-I through the full Diameter <-> HTTP/2
# translation pipeline across two h2diagent peers.
#
# Flow:
#   client h2agent (trigger) --HTTP/2--> client h2diagent --Diameter CCR-->
#     server h2diagent --HTTP/2--> server h2agent (mock CCA)
#   ... and the CCA-I travels back the same path.
#
# Assertions:
#   - server peer recorded the inbound CCR on /diameter/gx/CCR
#   - client peer recorded a 200 response carrying the mocked CCA
#     (Result-Code 2001) with the Session-Id echoed back.

import time

import pytest

from conftest import (
    ADMIN_SERVER_MATCHING_URI,
    ADMIN_SERVER_PROVISION_URI,
    ADMIN_SERVER_DATA_URI,
    ADMIN_CLIENT_ENDPOINT_URI,
    ADMIN_CLIENT_PROVISION_URI,
    ADMIN_CLIENT_DATA_URI,
)

GX_APP_ID = 16777238
GX_CCR_URI = "/diameter/gx/CCR"
SESSION_ID = "client.example.com;gxf-ccri;1"

# The client's h2agent posts outbound triggers to the client's own h2diagent
# HTTP/2 server, which lives in the same pod (localhost:8081).
CLIENT_HTTP2_SERVER_PORT = 8081

CLIENT_ENDPOINT = {
    "id": "diameter-gw",
    "host": "localhost",
    "port": CLIENT_HTTP2_SERVER_PORT,
    "secure": False,
}

# Server mock: answer any Gx CCR with a CCA carrying Result-Code 2001, echoing
# the session identifiers so we can correlate request and answer.
SERVER_PROVISION_CCA = {
    "requestMethod": "POST",
    "requestUri": GX_CCR_URI,
    "responseCode": 200,
    "responseHeaders": {"content-type": "application/json"},
    "responseBody": {
        "Session-Id": "",
        "Result-Code": 2001,
        "Origin-Host": "server.example.com",
        "Origin-Realm": "example.com",
        "Auth-Application-Id": GX_APP_ID,
        "CC-Request-Type": 1,
        "CC-Request-Number": 0,
    },
    "transform": [
        {"source": "request.body./Session-Id", "target": "response.body.json.string./Session-Id"},
        {"source": "request.body./CC-Request-Type", "target": "response.body.json.integer./CC-Request-Type"},
        {"source": "request.body./CC-Request-Number", "target": "response.body.json.unsigned./CC-Request-Number"},
    ],
}

# Client trigger: send a Gx CCR-I (CC-Request-Type=1, CC-Request-Number=0).
CLIENT_PROVISION_CCR = {
    "id": "send-gx-ccr",
    "endpoint": "diameter-gw",
    "requestMethod": "POST",
    "requestUri": GX_CCR_URI,
    "requestHeaders": {"content-type": "application/json"},
    "requestBody": {
        "Session-Id": SESSION_ID,
        "Origin-Host": "client.example.com",
        "Origin-Realm": "example.com",
        "Destination-Realm": "example.com",
        "Auth-Application-Id": GX_APP_ID,
        "CC-Request-Type": 1,
        "CC-Request-Number": 0,
    },
    "expectedResponseStatusCode": 200,
}


def _find_events(data, uri):
    """Return the outer client/server-data items matching a given uri."""
    if not isinstance(data, list):
        return []
    return [item for item in data if item.get("uri") == uri]


@pytest.mark.gx
@pytest.mark.functional
def test_gx_ccri_cca_i(server_admin, client_admin):
    # --- provision the server peer (mock CCA) ---
    status, _ = server_admin.post(ADMIN_SERVER_MATCHING_URI, {"algorithm": "FullMatching"})
    assert status in (200, 201), "server-matching setup failed: {}".format(status)

    status, _ = server_admin.post(ADMIN_SERVER_PROVISION_URI, SERVER_PROVISION_CCA)
    assert status == 201, "server-provision setup failed: {}".format(status)

    # --- provision the client peer (CCR trigger) ---
    status, _ = client_admin.post(ADMIN_CLIENT_ENDPOINT_URI, CLIENT_ENDPOINT)
    assert status in (200, 201), "client-endpoint setup failed: {}".format(status)

    status, _ = client_admin.post(ADMIN_CLIENT_PROVISION_URI, CLIENT_PROVISION_CCR)
    assert status == 201, "client-provision setup failed: {}".format(status)

    # --- fire it until the CCR reaches the server ---
    # Triggering returns 200 as soon as h2agent accepts the request, but that
    # does not guarantee the Diameter round-trip completed (the client<->server
    # CER/CEA peering may still be settling). Retrigger until the server records
    # the inbound CCR, then verify the answer on the client side.
    server_ccr = []
    trigger_attempts = 30
    for attempt in range(1, trigger_attempts + 1):
        status, body = client_admin.get(ADMIN_CLIENT_PROVISION_URI + "/send-gx-ccr")
        assert status == 200, "trigger failed: status={} body={}".format(status, body)
        for _ in range(5):  # ~2s window per trigger
            time.sleep(0.4)
            server_ccr = _find_events(server_admin.data_list(ADMIN_SERVER_DATA_URI), GX_CCR_URI)
            if server_ccr:
                break
        if server_ccr:
            break

    # --- assert on the server peer: it received the CCR ---
    assert server_ccr, (
        "server never recorded inbound CCR after {} triggers. "
        "client-data={}".format(trigger_attempts, client_admin.data_list(ADMIN_CLIENT_DATA_URI))
    )

    # --- assert on the client peer: it got the CCA with Result-Code 2001 ---
    client_ccr = _find_events(client_admin.data_list(ADMIN_CLIENT_DATA_URI), GX_CCR_URI)
    assert client_ccr, "client did not record the CCR event"

    inner = client_ccr[0].get("events", [{}])[0]
    response_body = inner.get("responseBody", {})
    assert response_body.get("Result-Code") == 2001, \
        "unexpected Result-Code in CCA: {}".format(response_body)
    assert response_body.get("Session-Id") == SESSION_ID, \
        "Session-Id not echoed back in CCA: {}".format(response_body)
    assert inner.get("clientProvisionId") == "send-gx-ccr"
