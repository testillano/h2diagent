# Gateway admin interface CT (/admin/v1 on h2diagent, port 8079 by default) and
# the client connection state gauge.
#
# Both peers run an h2diagent: the client peer dials the server peer (one
# Diameter connection), so each one lists the other in GET peers.

import os
import time
import urllib.request

import pytest

from conftest import AdminClient

GATEWAY_ADMIN_PORT = os.environ.get("GATEWAY_ADMIN_PORT", "8079")
GATEWAY_METRICS_PORT = os.environ.get("GATEWAY_METRICS_PORT", "8085")
SERVER_HOST = os.environ.get("SERVER_HOST", "server")
CLIENT_HOST = os.environ.get("CLIENT_HOST", "client")


@pytest.fixture(scope="module")
def server_gw():
    return AdminClient("server-gw", SERVER_HOST, GATEWAY_ADMIN_PORT)


@pytest.fixture(scope="module")
def client_gw():
    return AdminClient("client-gw", CLIENT_HOST, GATEWAY_ADMIN_PORT)


def open_connections(gw, leg, timeout=10):
    """Open connections of a leg, waiting for at least one (the CER/CEA may be in progress)."""
    deadline = time.time() + timeout
    while True:
        status, body = gw.get("/admin/v1/peers")
        assert status == 200
        found = [c for c in body["connections"] if c["leg"] == leg and c["state"] == "open"]
        if found or time.time() > deadline:
            return found
        time.sleep(0.2)


def test_001_health(server_gw, client_gw):
    for gw in (server_gw, client_gw):
        assert gw.get("/admin/v1/health") == (200, {"status": "healthy"})


def test_002_peers(server_gw, client_gw):
    client_leg = open_connections(client_gw, "client")
    assert len(client_leg) == 1
    c = client_leg[0]
    assert c["index"] == 0
    assert c["transport"] == "tcp"
    assert c["peerAddress"].endswith(":3868")
    assert c["peerOriginHost"] == "server.example.com"
    assert c["peerOriginRealm"] == "example.com"
    assert "localAddress" in c

    server_leg = open_connections(server_gw, "server")
    assert len(server_leg) == 1
    s = server_leg[0]
    assert s["peerOriginHost"] == "client.example.com"
    assert s["localAddress"].endswith(":3868")
    # the same connection seen from both ends (the source port; the address may be translated)
    assert s["peerAddress"].rsplit(":", 1)[1] == c["localAddress"].rsplit(":", 1)[1]


def test_003_configuration(client_gw):
    status, body = client_gw.get("/admin/v1/configuration")
    assert status == 200
    assert body["adminPort"] == int(GATEWAY_ADMIN_PORT)
    assert body["http2ServerPort"] == 8005
    assert body["metrics"]["prometheusPort"] == int(GATEWAY_METRICS_PORT)
    assert body["diameter"]["originHost"] == "client.example.com"
    assert body["diameter"]["peer"]["host"] == SERVER_HOST
    assert body["diameter"]["peer"]["connections"] == 1
    assert [d["applicationId"] for d in body["diameter"]["dictionaries"]] == [16777238]


def test_004_logging(client_gw):
    status, previous = client_gw.get("/admin/v1/logging")
    assert status == 200
    try:
        assert client_gw.request("PUT", "/admin/v1/logging?level=Debug")[0] == 200
        assert client_gw.get("/admin/v1/logging") == (200, "Debug")
        assert client_gw.request("PUT", "/admin/v1/logging?level=Loud")[0] == 400
        assert client_gw.get("/admin/v1/logging") == (200, "Debug")
    finally:
        client_gw.request("PUT", "/admin/v1/logging?level=" + previous)


def test_005_errors(client_gw):
    assert client_gw.get("/admin/v1/unknown")[0] == 404
    assert client_gw.request("POST", "/admin/v1/peers", {})[0] == 405


def test_006_connection_state_gauge(client_gw):
    assert open_connections(client_gw, "client")  # open (the gauge is refreshed every second)
    time.sleep(1.5)
    url = "http://{}:{}/metrics".format(CLIENT_HOST, GATEWAY_METRICS_PORT)
    with urllib.request.urlopen(url, timeout=5) as r:
        text = r.read().decode()
    samples = [l for l in text.splitlines() if l.startswith("h2diagent_diameter_client_connection_state{")]
    assert len(samples) == 1, samples
    assert 'connection="0"' in samples[0]
    assert float(samples[0].rsplit(" ", 1)[1]) == 1  # open


def test_007_capabilities_avps(server_gw, client_gw):
    # --cer (client) / --cea (server) from the CT chart values: shown as given, and
    # the capabilities exchange carrying them still opens the connection (the
    # bytes on the wire are checked by the diametercomm and h2diagent UT)
    status, body = client_gw.get("/admin/v1/configuration")
    assert status == 200
    assert body["diameter"]["cer"]["file"] == "/opt/capabilities/cer.json"
    assert body["diameter"]["cer"]["avps"] == {"Supported-Vendor-Id": [10415], "Firmware-Revision": 1,
                                               "Product-Name": "h2diagent"}
    assert "cea" not in body["diameter"]

    status, body = server_gw.get("/admin/v1/configuration")
    assert status == 200
    assert body["diameter"]["cea"]["avps"] == {"Supported-Vendor-Id": [10415], "Firmware-Revision": 2}

    assert open_connections(client_gw, "client")
    assert open_connections(server_gw, "server")

