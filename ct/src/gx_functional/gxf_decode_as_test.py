# Gx functional CT: --decode-as (readable decoding of OctetString AVPs).
#
# The SERVER h2diagent is deployed with (helm/ct-h2diagent values):
#   decodeAs: {Framed-IP-Address: ipv4, 3GPP-User-Location-Info: uli,
#              3GPP-MS-TimeZone: msTimeZone}
# the CLIENT h2diagent without it.
#
# Flow: the client sends a CCR-I with those AVPs written in readable forms
# (outbound forms, accepted by any gateway). Assertions:
#   - the server h2agent records them in readable form ({"ipv4": ...}), except
#     a Framed-IP-Address whose bytes do not fit an IPv4 (5 bytes): {"hex": ...};
#   - the server mock echoes the ULI it read back in the CCA; the client
#     gateway, without --decode-as, decodes it as plain hex.

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
SESSION_ID = "client.example.com;gxf-decode-as;1"
GX_CCR_SESSION_URI = GX_CCR_URI + "/" + SESSION_ID

ULI = {"type": "tai", "mcc": "460", "mnc": "00", "tac": 101}
ULI_HEX = "8064f0000065"

CLIENT_ENDPOINT = {"id": "diameter-gw", "host": "localhost", "port": 8081, "secure": False}

SERVER_PROVISION_CCA = {
    "requestMethod": "POST",
    "requestUri": GX_CCR_SESSION_URI,
    "responseCode": 200,
    "responseHeaders": {"content-type": "application/json"},
    "responseBody": {
        "Session-Id": SESSION_ID,
        "Result-Code": 2001,
        "Origin-Host": "server.example.com",
        "Origin-Realm": "example.com",
        "Auth-Application-Id": GX_APP_ID,
        "CC-Request-Type": 1,
        "CC-Request-Number": 0,
    },
    # Echo the ULI as read (readable form): it is encoded back to the same bytes.
    "transform": [
        {"from": {"source": "requestBody", "path": "/3GPP-User-Location-Info"},
         "to": {"target": "responseBody", "path": "/3GPP-User-Location-Info"}},
    ],
}

CLIENT_PROVISION_CCR = {
    "id": "send-gx-ccr-decode-as",
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
        "Framed-IP-Address": "0a8136a5ff",  # 5 bytes: not an IPv4 -> {"hex"} on the server
        "3GPP-User-Location-Info": {"uli": ULI},
        "3GPP-MS-TimeZone": {"msTimeZone": "+01:00"},
    },
    "expectedResponseStatusCode": 200,
}


def _find_events(data, uri):
    if not isinstance(data, list):
        return []
    return [item for item in data if item.get("uri") == uri]


@pytest.mark.gx
@pytest.mark.functional
def test_gx_decode_as_readable_forms(server_admin, client_admin):
    status, _ = server_admin.post(ADMIN_SERVER_MATCHING_URI, {"algorithm": "FullMatching"})
    assert status in (200, 201), "server-matching setup failed: {}".format(status)
    status, _ = server_admin.post(ADMIN_SERVER_PROVISION_URI, SERVER_PROVISION_CCA)
    assert status == 201, "server-provision setup failed: {}".format(status)
    status, _ = client_admin.post(ADMIN_CLIENT_ENDPOINT_URI, CLIENT_ENDPOINT)
    assert status in (200, 201), "client-endpoint setup failed: {}".format(status)
    status, _ = client_admin.post(ADMIN_CLIENT_PROVISION_URI, CLIENT_PROVISION_CCR)
    assert status == 201, "client-provision setup failed: {}".format(status)

    server_ccr = []
    for _ in range(30):
        status, body = client_admin.post(ADMIN_CLIENT_PROVISION_URI + "/send-gx-ccr-decode-as/trigger", None)
        assert status == 200, "trigger failed: status={} body={}".format(status, body)
        for _ in range(5):
            time.sleep(0.4)
            server_ccr = _find_events(server_admin.data_list(ADMIN_SERVER_DATA_URI), GX_CCR_SESSION_URI)
            if server_ccr:
                break
        if server_ccr:
            break
    assert server_ccr, "server never recorded the inbound CCR"

    # --- server (with --decode-as): readable forms, hex for what does not fit ---
    request_body = server_ccr[0].get("events", [{}])[0].get("requestBody", {})
    assert request_body.get("3GPP-User-Location-Info") == {"uli": ULI}, request_body
    assert request_body.get("3GPP-MS-TimeZone") == {"msTimeZone": "+01:00"}, request_body
    assert request_body.get("Framed-IP-Address") == {"hex": "0a8136a5ff"}, request_body

    # --- client (without --decode-as): the echoed ULI is plain hex ---
    client_ccr = []
    for _ in range(10):
        client_ccr = _find_events(client_admin.data_list(ADMIN_CLIENT_DATA_URI), GX_CCR_URI)
        if client_ccr:
            break
        time.sleep(0.4)
    assert client_ccr, "client did not record the CCR event"
    response_body = client_ccr[0].get("events", [{}])[0].get("responseBody", {})
    assert response_body.get("Result-Code") == 2001, response_body
    assert response_body.get("3GPP-User-Location-Info") == ULI_HEX, response_body
