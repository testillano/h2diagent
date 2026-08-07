# h2diagent component test - shared fixtures.
#
# The CT deploys two Diameter peers, each an h2diagent + h2agent sidecar pod,
# reachable by Kubernetes Service DNS name:
#
#   server (role=server) -> h2diagent listens Diameter :3868, mock brain = h2agent
#   client (role=client) -> h2diagent dials server:3868, mock brain = h2agent
#
# Tests talk to each peer's h2agent Admin REST API (HTTP/2) to provision mock
# behaviour, fire the traffic, and inspect the recorded data.

import collections.abc
import json
import logging
import os
import time

import pytest

# hyper needs these aliases for python 3.10+ compatibility.
collections.Iterable = collections.abc.Iterable
collections.Mapping = collections.abc.Mapping
collections.MutableSet = collections.abc.MutableSet
collections.MutableMapping = collections.abc.MutableMapping
from hyper import HTTP20Connection  # noqa: E402

#############
# CONSTANTS #
#############

ADMIN_URI_PREFIX = "/admin/v1/"
ADMIN_SERVER_MATCHING_URI = ADMIN_URI_PREFIX + "server-matching"
ADMIN_SERVER_PROVISION_URI = ADMIN_URI_PREFIX + "server-provision"
ADMIN_SERVER_DATA_URI = ADMIN_URI_PREFIX + "server-data"
ADMIN_CLIENT_ENDPOINT_URI = ADMIN_URI_PREFIX + "client-endpoint"
ADMIN_CLIENT_PROVISION_URI = ADMIN_URI_PREFIX + "client-provision"
ADMIN_CLIENT_DATA_URI = ADMIN_URI_PREFIX + "client-data"

logger = logging.getLogger("CT")


###########
# HELPERS #
###########

class AdminClient:
    """Thin HTTP/2 client for a single peer's h2agent Admin REST API."""

    def __init__(self, name, host, admin_port):
        self.name = name
        self.host = host
        self.admin_port = int(admin_port)
        self.authority = "{}:{}".format(host, admin_port)

    def request(self, method, path, body=None):
        """Perform an HTTP/2 request; return (status:int, body:dict|str|None)."""
        conn = HTTP20Connection(self.authority)
        headers = {"content-type": "application/json"}
        data = json.dumps(body) if body is not None else None
        conn.request(method, path, body=data, headers=headers)
        resp = conn.get_response()
        raw = resp.read()
        status = resp.status
        parsed = None
        if raw:
            try:
                parsed = json.loads(raw.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                parsed = raw.decode("utf-8", errors="replace")
        logger.debug("[%s] %s %s -> %s", self.name, method, path, status)
        return status, parsed

    # --- convenience wrappers ---
    def post(self, path, body):
        return self.request("POST", path, body)

    def get(self, path):
        return self.request("GET", path)

    def delete(self, path):
        return self.request("DELETE", path)

    def data_list(self, path):
        """GET a *-data endpoint; return a list ([] when empty / HTTP 204)."""
        status, body = self.get(path)
        if status == 204 or body is None:
            return []
        return body if isinstance(body, list) else []

    def clean(self):
        """Reset provisions and recorded data (best-effort, ignore status)."""
        for uri in (
            ADMIN_SERVER_PROVISION_URI,
            ADMIN_SERVER_DATA_URI,
            ADMIN_CLIENT_PROVISION_URI,
            ADMIN_CLIENT_DATA_URI,
        ):
            try:
                self.delete(uri)
            except Exception as exc:  # noqa: BLE001 - cleanup must never fail a test
                logger.warning("[%s] cleanup %s failed: %s", self.name, uri, exc)


############
# FIXTURES #
############

@pytest.fixture(scope="session")
def peers():
    """Peer endpoints, from wrapper-injected env vars (with sane defaults)."""
    return {
        "server": {
            "host": os.environ.get("SERVER_HOST", "server"),
            "admin_port": os.environ.get("SERVER_ADMIN_PORT", "8074"),
            "traffic_port": os.environ.get("SERVER_TRAFFIC_PORT", "8000"),
        },
        "client": {
            "host": os.environ.get("CLIENT_HOST", "client"),
            "admin_port": os.environ.get("CLIENT_ADMIN_PORT", "8074"),
            "traffic_port": os.environ.get("CLIENT_TRAFFIC_PORT", "8000"),
        },
    }


@pytest.fixture(scope="session")
def server_admin(peers):
    p = peers["server"]
    return AdminClient("server", p["host"], p["admin_port"])


@pytest.fixture(scope="session")
def client_admin(peers):
    p = peers["client"]
    return AdminClient("client", p["host"], p["admin_port"])


@pytest.fixture(autouse=True)
def clean_peers(server_admin, client_admin):
    """
    Isolate every test by wiping provisions/data *before* it runs.

    We intentionally do NOT clean afterwards, so the data recorded by the last
    test remains available for post-run inspection (see the hints printed by
    ct/test.sh). Isolation is still guaranteed because each test starts clean.
    """
    server_admin.clean()
    client_admin.clean()
    yield


@pytest.fixture(scope="session")
def trigger():
    """
    Fire a client provision and retry until it is sent successfully.

    The Diameter peering (CER/CEA) between client and server may take a moment
    after deployment, so the first trigger can transiently fail; retry with a
    small backoff.

    Returns a callable: trigger(client_admin, provision_id, retries, delay).
    """
    def _trigger(client_admin, provision_id, retries=20, delay=1.0):
        # h2agent triggers an on-demand client provision via GET on its id.
        uri = ADMIN_CLIENT_PROVISION_URI + "/" + provision_id
        last = None
        for attempt in range(1, retries + 1):
            status, body = client_admin.get(uri)
            last = (status, body)
            if status in (200, 201):
                logger.info("trigger '%s' ok on attempt %d", provision_id, attempt)
                return status, body
            logger.debug("trigger '%s' attempt %d -> %s", provision_id, attempt, status)
            time.sleep(delay)
        return last

    return _trigger
