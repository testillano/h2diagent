# Example: Gx CCR/CCA Mock (Credit-Control)

This example demonstrates a basic Gx session lifecycle mocked through h2diagent + h2agent:

```
Diameter Client         h2diagent            h2agent
(simulates GGSN)         (gateway)            (mock logic)
     |                      |                      |
     |-- CCR-I ------------>|-- POST /diameter/gx/CCR -->|
     |                      |                      | (provision: respond with CCA-I)
     |<-- CCA-I ------------|<-- 200 + JSON -------|
     |                      |                      |
     |-- CCR-U ------------>|-- POST /diameter/gx/CCR -->|
     |                      |                      | (in-state: update)
     |<-- CCA-U ------------|<-- 200 + JSON -------|
     |                      |                      |
     |-- CCR-T ------------>|-- POST /diameter/gx/CCR -->|
     |                      |                      | (in-state: termination)
     |<-- CCA-T ------------|<-- 200 + JSON -------|
```

## Files

- `dictionary-gx.json` - Gx Diameter dictionary for diametercodec
- `h2agent-provision.json` - h2agent server provision (mock CCR -> CCA logic)
- `h2agent-matching.json` - h2agent matching configuration
- `run.sh` - Script to run the full example

## How to run

```bash
# Terminal 1: start h2agent
docker run --rm -it --network host ghcr.io/testillano/h2agent:latest --verbose

# Terminal 2: start h2diagent
docker run --rm -it --network host ghcr.io/testillano/h2diagent:latest \
    --origin-host "mock-pcrf.example.com" \
    --origin-realm "example.com" \
    --dictionary examples/gx-ccr-cca/dictionary-gx.json \
    --h2agent-host localhost \
    --h2agent-port 8000 \
    --verbose

# Terminal 3: provision h2agent and send Diameter traffic
./examples/gx-ccr-cca/run.sh
```

## Understanding the h2agent provision

When h2diagent receives a Diameter CCR, it translates it to:

```
POST /diameter/gx/CCR HTTP/2
content-type: application/json
x-diameter-command-code: 272
x-diameter-application-id: 16777238

{
  "Session-Id": "ggsn.example.com;12345;67890",
  "Origin-Host": "ggsn.example.com",
  "Origin-Realm": "example.com",
  "CC-Request-Type": 1,
  "CC-Request-Number": 0,
  "Subscription-Id": {
    "Subscription-Id-Type": 1,
    "Subscription-Id-Data": "24001000001"
  }
}
```

h2agent matches on `POST /diameter/gx/CCR` and uses `CC-Request-Type` from the
request body to drive state transitions:
- Type 1 (INITIAL) -> respond with charging rules, out-state "update"
- Type 2 (UPDATE) -> respond with updated rules, out-state "termination"
- Type 3 (TERMINATION) -> respond with Result-Code 2001, out-state "purge"

The JSON response is translated back by h2diagent into a Diameter CCA.
