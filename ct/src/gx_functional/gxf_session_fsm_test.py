# Gx functional CT: per-session state machine for inbound Diameter requests.
#
# h2diagent forwards every inbound request as
#   POST /diameter/<interface>/<command>/<percent-encoded Session-Id>
# so the h2agent state (inState/outState), keyed by the received URI, is per
# session. One generic provision chain per command is kept with
# FullMatchingRegexReplace (the Session-Id segment is dropped for
# classification only).
#
# Flow (sessions A and B interleaved: A, B, A, B):
#   server chain: initial --(2001)--> second --(2002)--> purge
#   expected Result-Codes: A=2001, B=2001, A=2002, B=2002
#   (a single shared state machine would give 2001, 2002, then no provision).
#
# The client provisions also use the per-session URI shape
# (/diameter/gx/CCR/<Session-Id>): the outbound parser ignores the suffix.

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
CLIENT_HTTP2_SERVER_PORT = 8081

SESSIONS = {
    "warmup": "client.example.com;gxf-fsm;0",
    "A": "client.example.com;gxf-fsm;1",
    "B": "client.example.com;gxf-fsm;2",
}

CLIENT_ENDPOINT = {
    "id": "diameter-gw",
    "host": "localhost",
    "port": CLIENT_HTTP2_SERVER_PORT,
    "secure": False,
}

# Classification drops everything after the command (Session-Id may contain
# '/' once decoded, so take the whole tail, not just the next segment).
SERVER_MATCHING = {
    "algorithm": "FullMatchingRegexReplace",
    "rgx": "(/diameter/[^/]+/[^/]+)/.*",
    "fmt": "$1",
}


def _server_provision(in_state, out_state, result_code):
    return {
        "inState": in_state,
        "outState": out_state,
        "requestMethod": "POST",
        "requestUri": GX_CCR_URI,
        "responseCode": 200,
        "responseHeaders": {"content-type": "application/json"},
        "responseBody": {
            "Session-Id": "",
            "Result-Code": result_code,
            "Origin-Host": "server.example.com",
            "Origin-Realm": "example.com",
            "Auth-Application-Id": GX_APP_ID,
            "CC-Request-Type": 1,
            "CC-Request-Number": 0,
        },
        "transform": [
            {"from": {"source": "requestBody", "path": "/Session-Id"},
             "to": {"target": "responseBody", "path": "/Session-Id"}},
            {"from": {"source": "requestBody", "path": "/CC-Request-Type"},
             "to": {"target": "responseBody", "path": "/CC-Request-Type"}},
            {"from": {"source": "requestBody", "path": "/CC-Request-Number"},
             "to": {"target": "responseBody", "path": "/CC-Request-Number"}},
        ],
    }


SERVER_PROVISIONS = [
    _server_provision("initial", "second", 2001),
    _server_provision("second", "purge", 2002),
]


def _client_uri(name):
    return GX_CCR_URI + "/" + SESSIONS[name]


def _client_provision(name):
    return {
        "id": "ccr-" + name,
        "endpoint": "diameter-gw",
        "requestMethod": "POST",
        "requestUri": _client_uri(name),
        "requestHeaders": {"content-type": "application/json"},
        "requestBody": {
            "Session-Id": SESSIONS[name],
            "Origin-Host": "client.example.com",
            "Origin-Realm": "example.com",
            "Destination-Realm": "example.com",
            "Auth-Application-Id": GX_APP_ID,
            "CC-Request-Type": 1,
            "CC-Request-Number": 0,
        },
    }


def _client_events(client_admin, name):
    data = client_admin.data_list(ADMIN_CLIENT_DATA_URI)
    for item in data:
        if item.get("uri") == _client_uri(name):
            return item.get("events", [])
    return []


def _fire(client_admin, name, expected_events, timeout=5.0):
    """Trigger once and wait until the client records the answered event."""
    status, body = client_admin.post(ADMIN_CLIENT_PROVISION_URI + "/ccr-" + name + "/trigger", None)
    assert status == 200, "trigger {} failed: status={} body={}".format(name, status, body)
    deadline = time.time() + timeout
    while time.time() < deadline:
        events = _client_events(client_admin, name)
        if len(events) >= expected_events and "responseStatusCode" in events[expected_events - 1]:
            return events[expected_events - 1]
        time.sleep(0.2)
    return None


def _result_code(event):
    return (event or {}).get("responseBody", {}).get("Result-Code")


@pytest.mark.gx
@pytest.mark.functional
def test_gx_inbound_state_machine_per_session(server_admin, client_admin):
    status, _ = server_admin.post(ADMIN_SERVER_MATCHING_URI, SERVER_MATCHING)
    assert status in (200, 201), "server-matching setup failed: {}".format(status)
    status, _ = server_admin.post(ADMIN_SERVER_PROVISION_URI, SERVER_PROVISIONS)
    assert status == 201, "server-provision setup failed: {}".format(status)

    status, _ = client_admin.post(ADMIN_CLIENT_ENDPOINT_URI, CLIENT_ENDPOINT)
    assert status in (200, 201), "client-endpoint setup failed: {}".format(status)
    for name in SESSIONS:
        status, _ = client_admin.post(ADMIN_CLIENT_PROVISION_URI, _client_provision(name))
        assert status == 201, "client-provision {} setup failed: {}".format(name, status)

    # Warm-up on its own session until the Diameter peering is settled (its
    # state machine is independent, so retries do not disturb A and B).
    ready = False
    for attempt in range(1, 31):
        event = _fire(client_admin, "warmup", attempt, timeout=2.0)
        if event and event.get("responseStatusCode") == 200:
            ready = True
            break
    assert ready, "Diameter peering never settled: {}".format(_client_events(client_admin, "warmup"))

    # Interleaved sessions: each one walks its own state machine.
    for name, nth, expected in (("A", 1, 2001), ("B", 1, 2001), ("A", 2, 2002), ("B", 2, 2002)):
        event = _fire(client_admin, name, nth)
        assert event is not None, "no answer for session {} (#{})".format(name, nth)
        assert event.get("responseStatusCode") == 200, "session {} (#{}): {}".format(name, nth, event)
        assert _result_code(event) == expected, \
            "session {} (#{}): expected Result-Code {}, got {}".format(name, nth, expected, event)
        assert event["responseBody"].get("Session-Id") == SESSIONS[name]

    # Server data: both per-session keys reached the final 'purge' state, so
    # they were removed; nothing is left under the bare command URI either.
    server_uris = {item.get("uri") for item in server_admin.data_list(ADMIN_SERVER_DATA_URI)}
    assert GX_CCR_URI + "/" + SESSIONS["A"] not in server_uris
    assert GX_CCR_URI + "/" + SESSIONS["B"] not in server_uris
    assert GX_CCR_URI not in server_uris
