# C++ HTTP/2 - DIAMETER Gateway Service

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/testillano/h2diagent/actions/workflows/ci.yml/badge.svg)](https://github.com/testillano/h2diagent/actions/workflows/ci.yml)
[![Documentation](https://codedocs.xyz/testillano/h2diagent.svg)](https://codedocs.xyz/testillano/h2diagent/index.html)

`h2diagent` is a lightweight **Diameter <-> HTTP/2 translation gateway** that enables [h2agent](https://github.com/testillano/h2agent) to mock and generate Diameter traffic without native Diameter support.

It translates Diameter messages to/from JSON over HTTP/2, allowing h2agent's full translation engine (sources, filters, targets, FSM, schemas) to drive Diameter test scenarios for interfaces like Gx, Rx, Sy, etc.

## Architecture

```
                    +----------------------------------+
                    |          h2diagent               |
                    |                                  |
  SUT <--Diameter-->|  Diameter server (:3868)         |
                    |  Diameter client (-> SUT)        |
                    |                                  |
                    |  HTTP/2 client  (-> h2agent)     |<-- inbound Diameter
                    |  HTTP/2 server  (:8080)          |<-- outbound Diameter
                    +----------------------------------+
                                    | HTTP/2
                    +----------------------------------+
                    |           h2agent                |
                    |  Traffic server (:8000) <- mock  |
                    |  Traffic client -> h2diagent     |
                    |  Admin API (:8074)               |
                    +----------------------------------+
```

## How it works

**Inbound flow** (SUT -> mock):
1. SUT sends Diameter request (e.g., CCR) to h2diagent
2. h2diagent decodes AVPs and maps them to JSON
3. Forwards as HTTP/2 POST to h2agent (e.g., `POST /diameter/gx/CCR/<Session-Id>`, see [inbound URI](#inbound-uri-session-id-and-per-session-state))
4. h2agent processes with provisions/transforms, responds with JSON
5. h2diagent maps JSON back to Diameter AVPs and sends answer to SUT

**Outbound flow** (mock -> SUT):
1. h2agent client sends HTTP/2 POST to h2diagent (e.g., `POST /diameter/rx/RAR`; a trailing `/<anything>`, such as the Session-Id, is accepted and ignored)
2. h2diagent maps JSON to Diameter AVPs and sends request to SUT
3. SUT responds with Diameter answer
4. h2diagent maps answer to JSON and returns HTTP/2 response to h2agent

### Inbound URI: Session-Id and per-session state

Inbound requests are forwarded as

```
POST /diameter/<interface>/<command>/<percent-encoded Session-Id>
```

e.g. `POST /diameter/rx/RAR/af.realm.com%3B004001010020000%3B1630322271`.
h2agent decodes the path, so its matching key and server data URI hold the
Session-Id verbatim (`/diameter/rx/RAR/af.realm.com;004001010020000;1630322271`).

Why: h2agent keys the provision state machine (`inState`/`outState`) and the
event history by method + **received** URI. With a fixed URI per command all
sessions would share one state machine; with the Session-Id in the path each
session evolves its own (e.g. a RAR chain per session).

> **Breaking change (2.0.0).** A provision keyed by the bare
> `/diameter/<interface>/<command>` no longer matches with `FullMatching`.
> Choose the classification:
>
> | Goal | server-matching | Provisions |
> |------|-----------------|------------|
> | One generic provision chain per command, state per session | `{"algorithm": "FullMatchingRegexReplace", "rgx": "(/diameter/[^/]+/[^/]+)/.*", "fmt": "$1"}` | `requestUri: "/diameter/rx/RAR"` |
> | One literal provision per session (Session-Ids known in advance) | `{"algorithm": "FullMatching"}` | `requestUri: "/diameter/rx/RAR/<Session-Id>"` (decoded) |
>
> The regex takes everything after the command, not just the next segment: a
> Session-Id containing `/` yields extra path segments once decoded. When the
> regex does not match (no Session-Id), the URI is kept as is, so the same
> recipe also covers bare URIs. The state machine is per received URI in both
> cases (see h2agent [classification pipeline](https://github.com/testillano/h2agent/blob/master/docs/api/README.md#classification-pipeline)).

Details:

- **Encoding**: every byte outside RFC 3986 unreserved (`A-Z a-z 0-9 - . _ ~`)
  is percent-encoded (`;` -> `%3B`, `/` -> `%2F`, UTF-8 byte-wise).
- **Extraction**: the Session-Id (AVP 263) is read raw from the wire, before
  and independently of the decode (AVP header hops only, normally the first
  AVP). An undecodable request (`{_raw, _decodeError, _header}`) still gets its
  per-session URI unless the framing is broken before the Session-Id. The same
  value is echoed in the 5012 answer.
- **No Session-Id** (base protocol messages never reach h2agent; otherwise a
  SUT bug / negative test): the bare `/diameter/<interface>/<command>` is used.
- Transforms can read the Session-Id from the body (as before) or from the URI
  (`requestUri` source + `regexReplace`).
- Admin queries on server data must percent-encode the URI query parameter
  (`requestUri=...`), as `&` or `=` inside a Session-Id would break the query.
- In load tests every session creates its own server data key: end the chain
  with `purge`, or use a lighter data storage configuration, to bound memory.

### Fail-fast when the Diameter peer is down (`502 Bad Gateway`)

If an outbound `POST /diameter/<interface>/<command>` arrives while h2diagent has
**no open Diameter peer** to the stack (e.g. right after a fresh deploy, while the
peer connection is still being (re)established, or after the peer dropped), the
gateway does **not** block the request or drop it silently. It responds immediately
with:

- **`502 Bad Gateway`** — the gateway-to-upstream failure code (there is no usable
  Diameter path to send the request on). `503` is intentionally reserved for the
  gateway's own overload, and `504` for an in-flight answer timeout; `502` here means
  "no usable peer at all, nothing was sent".
- Header **`x-diameter-peer-state: closed`** — a diagnostic hint reporting the peer
  condition that caused the refusal, so the caller knows *why* without reading gateway
  logs. The JSON body mirrors it (`{"error":"Diameter peer not connected","peerState":"closed"}`).

The gateway is **policy-free** here: it only reports. It does **not** retry on behalf
of the client, does **not** buffer the request, and — deliberately — does **not** emit
an authoritative `Retry-After` header or any back-off hint. The retry/abort policy is
left entirely to the client (the h2agent director that issued the POST), which can
react to the `502` in its `client-provision` as it sees fit (retry with its own
back-off, give up with a verdict, or escalate). This keeps the gateway generic and the
behaviour per-flow explicit, giving the client full freedom over timing.

### Every outbound request gets an HTTP/2 answer

The gateway always completes the HTTP/2 stream of an outbound request, so the
director never waits for its own client timeout (h2agent `responseStatusCode -2`):

| Status | Body | When |
|--------|------|------|
| `200` | answer JSON (AVPs + `_header`) | Diameter answer received and decoded |
| `400` | `{"error":"Invalid JSON: ..."}` / `{"error":"Invalid _raw: ..."}` | body cannot be parsed or encoded (e.g. unknown AVP name) |
| `502` | `{"error":"Diameter peer not connected","peerState":"closed"}` + `x-diameter-peer-state: closed` | no open Diameter peer (see above) |
| `502` | `{"error":"Diameter send failed"}` (+ `x-diameter-peer-state: closed` if the peer is down) | the connection refused the write |
| `502` | `{"error":"Failed to decode Diameter answer","_raw":"<hex>","_decodeError":"...","_header":{...}}` | the answer arrived but is malformed |
| `504` | `{"error":"Diameter answer timeout","hopByHop":"0x...","timeoutMs":N}` | no answer within `--diameter-timeout-ms` |

Error bodies are always valid JSON (exception texts are escaped).

Inbound requests (peer -> h2agent) are always answered too: when h2agent is not
reachable, answers non-200, times out, or returns a body that cannot be encoded,
the gateway sends `DIAMETER_UNABLE_TO_COMPLY` (5012), echoing the request
`Session-Id`.

## Answer correlation

An outbound request is correlated with its answer by **hop-by-hop** on the
connection that sent it. Two fallbacks cover stacks that do not return the
answer exactly there:

- **End-to-end fallback.** RFC 6733 keeps the End-to-End Identifier unchanged
  along the path (relays rewrite and restore only the hop-by-hop). If no pending
  transaction has the answer hop-by-hop (or the command code differs), the
  end-to-end is used. The gateway assigns a node-wide unique end-to-end to every
  request, so it is a safe key. The command code must always match.
- **Cross-leg / cross-connection.** The gateway has a client leg (it dials the
  peer) and a server leg (the peer dials it), both with the same Origin-Host.
  For the stack this is one peer with several connections, and it may send an
  answer on any of them (e.g. a deferred STA on the connection it opened for
  RAR/ASR). An answer that misses on its connection is tried on the other pooled
  client connections and on the other leg before being dropped.

An answer that matches nothing (late after a timeout, duplicated, or unknown)
is **dropped and logged at Warning** (leg, command, application, hop-by-hop,
end-to-end). It is **never** forwarded to h2agent as if it were a request.
See the `*_answers_uncorrelated_counter` and `*_answers_recovered_counter`
metrics.

## Diameter <-> JSON mapping

This section describes how each Diameter AVP **data format** is represented in
the JSON that h2diagent exchanges with h2agent:

- **client-provision** requests (h2agent -> h2diagent -> Diameter peer), and
- **server-provision** responses (Diameter peer -> h2diagent -> h2agent).

### Model: representation is driven by the AVP format in the dictionary

The JSON is **flat**: each AVP is a key (its dictionary name) mapping to a value.
There is **no per-value type/encoding flag**. The representation is **type-driven**:
the engine looks up the AVP's **format** in the loaded dictionary (`--dictionary`)
and encodes/decodes the JSON value according to the Diameter norm for that format.

Rules of thumb:
- **Numeric** formats -> JSON number, unquoted.
- **Textual** formats -> JSON string (human text).
- **OctetString** (opaque) -> JSON string containing **hex** (even number of
  hex digits).
- **Grouped** -> JSON object (repeated AVPs become a JSON array).

The AVP name is the JSON key. Vendor-specific AVPs are resolved by the
dictionary (the Vendor-Id / V-bit is taken from the dictionary entry); you only
write the AVP name, never the Vendor-Id.

### The `_header` reserved key (message header)

Besides AVPs, the JSON carries one **reserved key, `_header`**, that represents
the **Diameter message header** (version, flags, command-code, application-id,
request/answer, and the transaction identifiers). It is **not** an AVP: any key
named `_header` is treated as the header and is never encoded as an AVP.

```json
"_header": {
  "version": 1,
  "flags": 128,
  "command-code": 272,
  "request": true,
  "application-id": 16777238,
  "hop-by-hop-id": 0,
  "end-to-end-id": 0
}
```

| Field            | Type    | Meaning |
|------------------|---------|---------|
| `version`        | number  | Diameter version (always `1`). |
| `flags`          | number  | Command flags **bitmask**: `0x80` R (request), `0x40` P (proxiable), `0x20` E (error), `0x10` T (potentially re-transmitted). |
| `command-code`   | number  | Command code, e.g. `272` (CC), `265` (AA), `258` (RA), `275` (ST). |
| `request`        | boolean | `true` = request (sets the R-bit), `false` = answer. Overrides the R-bit in `flags`. |
| `application-id` | number  | Diameter Application-Id, e.g. `16777238` (Gx). |
| `hop-by-hop-id`  | number  | Transaction correlation id; `0` lets the client assign it. |
| `end-to-end-id`  | number  | End-to-end id; `0` lets the client assign it. |

When h2diagent **decodes** an inbound Diameter message to JSON (before POSTing
to h2agent), it always emits `_header` alongside the AVPs, so provisions can
read/match/transform on it. When h2diagent **encodes** JSON back to Diameter, it
reads `_header` if present (missing fields fall back to defaults).

#### The header is auto-managed; `_header` in a provision is an optional override

In normal operation you do **not** need to write `_header`: h2diagent fills it in
for you. Provide it only when you want to override the header.

> **Important - `_header` is used as-is (no merge).** If a provision includes
> `_header`, h2diagent uses it **verbatim** and does **not** merge it with the
> values it would otherwise compute. Any field you omit falls back to its default
> (`0` for the numeric ids). So a *partial* `_header` (e.g. only `flags`) will
> reset `command-code`, `application-id`, `hop-by-hop-id` and `end-to-end-id` to
> `0` -- which, for an answer, breaks correlation at the peer. If you override,
> provide the **complete** header.

- **Server-provision responses (inbound answer, peer -> h2agent -> peer).**
  The HTTP/2 response is correlated with the request at the stream level, and
  h2diagent already holds the original `command-code`, `application-id`,
  `hop-by-hop-id` and `end-to-end-id`. So whenever your response body has **no**
  `_header`, it **reconstructs the answer header from that request context**
  (with `request:false`, `flags:0`). This is the normal, recommended case: you
  do not mirror anything back (the HTTP/2 response headers are ignored).

  Because the answer's `hop-by-hop-id`/`end-to-end-id` are **dynamic** (per
  transaction) and a static provision does not know them, the practical way to
  override a header field (e.g. set the Error bit) is to **echo the request's
  `_header`** -- which is present in the inbound request JSON -- via an h2agent
  transform, and then adjust the field you want. Hand-writing a static `_header`
  in the response is only safe if you supply the correct dynamic ids too.

- **Client-provision requests (outbound, h2agent -> h2diagent -> peer).**
  When the request body has no `_header`, h2diagent **derives it from the request
  URI** `/diameter/<interface>/<command>` (mapping the interface to the
  Application-Id and the command abbreviation to the command-code; anything
  after the command, e.g. `/<Session-Id>`, is ignored), sets
  `request:true` / `flags:0x80`, and leaves `hop-by-hop-id`/`end-to-end-id` at
  `0` for the client to assign. Overriding here is practical: provide a
  **complete** `_header` to force a command-code/application-id the URI cannot
  express, or extra flags (`hop-by-hop-id`/`end-to-end-id` can stay `0`).

  ```json
  {
    "Session-Id": "client.example.com;sess;1",
    "Origin-Host": "client.example.com",
    "_header": {
      "version": 1, "flags": 128, "request": true,
      "command-code": 272, "application-id": 16777238,
      "hop-by-hop-id": 0, "end-to-end-id": 0
    }
  }
  ```

#### Related: `x-diameter-*` request headers (inbound POST to h2agent)

On the inbound flow, h2diagent also sets informational HTTP headers on the POST
to h2agent: `x-diameter-command-code`, `x-diameter-application-id`,
`x-diameter-hop-by-hop`, `x-diameter-end-to-end`, `x-diameter-flags`. They mirror
`_header` for convenience (matching, transforms, logging) and are **optional to
use**. They are not required on the response: the gateway does not read the
HTTP/2 response headers, because it rebuilds the answer header from the
correlated request as described above.

### AVP order on the wire

JSON objects have no order (h2agent serializes keys alphabetically), so the
gateway encodes AVPs in the **dictionary command order** (the command ABNF:
fixed AVPs such as `< Session-Id >` first, then the rules as declared), at
message level and inside Grouped AVPs. AVPs not covered by any rule (including
raw `avp-<code>` ones) go last; repeated AVPs keep their JSON array order. If
the dictionary has no rules for the command, the JSON order is kept.

> The dictionary must flag request commands with `"r-bit": true` (requests and
> answers share the command code). Without it the request rules are not found
> and no reordering happens. The bundled `tools/stacks/*.json` include it.

### Per-format table (the 16 base/derived formats)

| Diameter format   | JSON type | Example (JSON)                         | On-the-wire meaning |
|-------------------|-----------|----------------------------------------|---------------------|
| Integer32         | number    | `"CC-Request-Number": 0`               | signed 32-bit |
| Integer64         | number    | `"Accounting-Sub-Session-Id": 12345`   | signed 64-bit |
| Unsigned32        | number    | `"Auth-Application-Id": 16777238`      | unsigned 32-bit |
| Unsigned64        | number    | `"CC-Total-Octets": 1099511627776`     | unsigned 64-bit |
| Float32           | number    | `"Some-Float32": 1.5`                  | IEEE 754 single |
| Float64           | number    | `"Some-Float64": 1.5`                  | IEEE 754 double |
| Enumerated        | number    | `"CC-Request-Type": 1`                 | signed 32-bit (enum value; no alias in flat JSON) |
| OctetString       | string (hex), or a readable form (object) | `"Framed-IP-Address": "c0a80001"` / `{"ipv4": "192.168.0.1"}` | raw bytes; `"af01"` -> 0xAF 0x01 |
| UTF8String        | string    | `"Subscription-Id-Data": "1234567890"` | UTF-8 text |
| DiameterIdentity  | string    | `"Origin-Host": "pcef.example.com"`    | text |
| DiameterURI       | string    | `"Redirect-Server-Address": "aaa://host:3868"` | text |
| IPFilterRule      | string    | `"IPFilterRule-Avp": "permit in ip from any to any"` | text |
| QoSFilterRule     | string    | `"QoSFilterRule-Avp": "..."`           | text |
| Time              | number    | `"Event-Timestamp": 1700000000`        | UNIX epoch seconds (engine converts to/from NTP) |
| Address           | string    | `"Some-Address": "192.168.0.2"`        | IPv4/IPv6 human form; engine encodes family+bytes on the wire |
| Grouped           | object    | see below                              | nested AVPs |

#### OctetString (hex)

OctetString carries opaque bytes, so it is written as a **hex string** (even
length). Examples:
```json
"Framed-IP-Address": "c0a80001",          // 4 raw bytes = 192.168.0.1
"3GPP-User-Location-Info": "8064f0000064" // packed binary, verbatim bytes
```
`"af01"` decodes to the two bytes 0xAF 0x01. The hex must be valid and even
length; invalid hex is an error (do not rely on silent parsing).

**Readable forms (outbound JSON only).** Instead of the hex, an OctetString may
be written as an object with exactly ONE key, which names how its bytes are
built. The key never depends on the AVP: which form suits an AVP is its 3GPP
definition, as it is when writing the hex by hand. The wire bytes are the same
as with the equivalent hex, and decoded messages keep giving the hex back.

| Form | Bytes | Typical AVPs | Example |
|---|---|---|---|
| `{"hex": "..."}` | the hex bytes (same as the plain string) | any | `{"hex": "0a8136a5"}` |
| `{"text": "..."}` | UTF-8 bytes of the text | `AF-Application-Identifier`, `Called-Station-Id`, `AF-Charging-Identifier` | `{"text": "dnn_mbb.com"}` |
| `{"ipv4": "..."}` | 4 bytes of the address | `Framed-IP-Address` | `{"ipv4": "10.129.54.165"}` |
| `{"ipv6": "..."}` | 16 bytes of the address | IPv6 addresses held in OctetStrings | `{"ipv6": "2001:db8::1"}` |
| `{"ipv6Prefix": "<addr>/<len>"}` | RFC 3162: reserved byte, length, prefix bytes | `Framed-IPv6-Prefix` | `{"ipv6Prefix": "2001:db8:1:2::/64"}` |
| `{"msTimeZone": "+-hh:mm[+dst]"}` | TS 29.061: TS 24.008 time zone (quarters of an hour, BCD, sign bit) + DST hours (0..2) | `3GPP-MS-TimeZone` | `{"msTimeZone": "-08:00+1"}` (`2b01`) |
| `{"uli": {...}}` | TS 29.061: location type + BCD MCC/MNC + TAC / ECI | `3GPP-User-Location-Info` | see below |

```json
"AF-Application-Identifier": {"text": "urn%3Aurn-xxx%3A3gpp-service.ims.icsi.mmtel"},
"Framed-IP-Address": {"ipv4": "10.129.54.165"},
"3GPP-User-Location-Info": {"uli": {"type": "tai", "mcc": "460", "mnc": "00", "tac": 101}},
"3GPP-User-Location-Info": {"uli": {"type": "tai-ecgi", "mcc": "214", "mnc": "07", "tac": 1, "eci": "0x3039"}}
```

`uli` types: `tai` (128: `tac`), `ecgi` (129: `eci`, 28 bits) and `tai-ecgi`
(130: both). `mcc` is 3 digits and `mnc` 2 or 3 (strings: leading zeros
matter); `tac` / `eci` are decimal numbers or `"0x..."` hex strings. An invalid
form is an encoding error, as an invalid hex is. Address-format AVPs (e.g.
`Host-IP-Address`) are not OctetStrings: they already take the human form.

IMPORTANT for IP-bearing OctetStrings: many 3GPP AVPs (e.g. `Framed-IP-Address`,
AVP code 8) are declared **OctetString** and carry the **4 raw bytes** of the
IPv4 address per the norm. Therefore they must be written as hex of those 4
bytes (192.168.0.2 -> `"c0a80002"`), NOT as the dotted-decimal string
`"192.168.0.2"` and NOT as the ASCII of the dotted string. Writing a
dotted-decimal string into an OctetString produces wrong bytes and the peer will
reject the message (e.g. Result-Code 5004 DIAMETER_INVALID_AVP_VALUE).

#### Grouped

Repeated inner AVPs are expressed as a JSON array under the AVP name:
```json
"Subscription-Id": [
  { "Subscription-Id-Type": 1, "Subscription-Id-Data": "1234567890" }
],
"Supported-Features": [
  { "Vendor-Id": 10415, "Feature-List-ID": 1, "Feature-List": 11 }
]
```
On decode, a grouped AVP that appears multiple times is rendered as an array of
objects.

#### Time

Represented as **UNIX epoch seconds** (a plain number). The engine converts
to/from the Diameter NTP timestamp (adds/subtracts the NTP epoch offset).

### Caveats / notes (engine)

- **Address format**: handled per the Diameter norm. In JSON it is the human
  form (dotted IPv4 / IPv6 literal); on the wire the engine builds
  `address-family (2 bytes) + address bytes` (IPv4 = family 1 + 4 bytes,
  IPv6 = family 2 + 16 bytes) via inet_pton/inet_ntop.
  Note: `Framed-IP-Address` (AVP 8) is NOT `Address` format - it is declared
  `OctetString` (4 raw bytes), so it uses hex, not the dotted form.
- **Enumerated**: represented as the integer value in JSON; enumerated
  **aliases** (symbolic names) are not part of the JSON, but they ARE shown in
  the debug traces (see below).
- **OctetString hex**: must be strict, even-length hex.

### Debug traces (per-AVP format mapping)

When h2diagent runs with `--log-level Debug`, the codec emits a trace for every
AVP as it crosses between HTTP/2 (JSON) and Diameter (bytes), showing the
format-driven mapping. Examples:

```
Diameter encode AVP 'Framed-IP-Address' (OctetString): 0xc0a80001 -> 0xc0a80001
Diameter encode AVP 'CC-Request-Type' (Enumerated): 1 (INITIAL_REQUEST) -> 0x00000001
Diameter decode AVP 'Result-Code' (Unsigned32): 0x00001391 -> 5004
Diameter decode AVP 'Origin-Host' (DiameterIdentity): 0x... -> "pcrf.example.com"
```

For `Enumerated` AVPs the trace appends the dictionary alias/literal in
parentheses next to the numeric value.

### JSON mapping summary

Write each AVP value in the canonical representation for its dictionary format:
numbers for integer/unsigned/float/enum, human text for the text formats, UNIX
epoch for Time, hex for OctetString (or one of its readable forms: `text`,
`ipv4`, `ipv6`, `ipv6Prefix`, `msTimeZone`, `uli`), and nested objects/arrays
for Grouped. The
engine encodes to the normative Diameter wire bytes according to that format.

## Negative testing

The codec does not enforce command rules or value ranges, so many negative
cases need nothing special:

| Case | How | Typical SUT result |
|------|-----|--------------------|
| Missing mandatory AVP | omit the key | 5005 MISSING_AVP |
| AVP not allowed / extra AVP | add any dictionary AVP | 5008 / ignored |
| Too many occurrences | JSON array with N elements | 5009 |
| Invalid value (enum out of range, bad text) | write the value | 5004 |
| Wrong header (application-id, command-code, version, E/P/T flags) | complete `_header` | 3007, 3001, 5011 ... |
| Error answer from the mock | `Result-Code` / `Experimental-Result` (+ E-bit in `_header`) | SUT error handling |
| No answer from the mock | provision delay above the SUT timeout | SUT timeout handling |

For what the dictionary format cannot express, two raw hooks exist:

**Raw AVP: `avp-<code>` / `avp-<code>-<vendor>` keys** (any level, also inside
Grouped). These are the names the decoder already emits for unknown AVPs, so an
inbound unknown AVP can be echoed back. The data is encoded verbatim:

```json
"avp-99999": "0000000a",                          // unknown AVP, no flags
"avp-416": { "hex": "0001", "flags": 0 },         // known AVP, 2-byte data, M-bit off
"avp-1027-10415": { "hex": "00000001", "flags": 192 }
```

- string value: hex of the data bytes; flags = V-bit when a vendor is given.
- object value: `hex` plus optional `flags` (full AVP flags byte, 0..255; the
  V-bit decides whether the Vendor-Id is written).
- The AVP length is computed from the data and padding is added as usual.

Covers unknown AVPs (5001 with M-bit set), wrong flags, wrong data length
(5014 INVALID_AVP_LENGTH) and invalid encodings (5004).

**Raw message: `_raw` key** (hex of the whole Diameter message), in a
client-provision request body or in a server-provision answer. All other keys
are ignored and the bytes are sent verbatim, except a hop-by-hop / end-to-end
equal to 0, which is filled as usual (the client assigns it for requests; the
request ids are used for answers) so correlation keeps working. At least the
20-byte header is required (otherwise `400`).

```json
{ "_raw": "0100002c800001100100001600000000000000000000010740..." }
```

> Warning: a message length that does not match the bytes breaks TCP framing
> for the whole connection (the peer waits for more bytes or reads garbage).
> Use it at the end of a test, and expect the peer to answer 5015
> INVALID_MESSAGE_LENGTH or close the connection.

**Malformed inbound messages.** When an inbound request cannot be decoded, it
is still POSTed to h2agent on the usual URI with the body
`{"_raw":"<hex>","_decodeError":"<reason>","_header":{...}}` (the header is
read from the first 20 bytes). The provision decides the answer, e.g. a 5014
via `_raw` or plain JSON. Without a matching provision the usual 5012 applies.
Malformed answers to outbound requests are reported the same way inside the
`502` body (see above).

## Dependencies

| Component | Repository |
|-----------|-----------|
| Diameter codec | [testillano/diametercodec](https://github.com/testillano/diametercodec) |
| Diameter communications | [testillano/diametercomm](https://github.com/testillano/diametercomm) |
| HTTP/2 communications | [testillano/http2comm](https://github.com/testillano/http2comm) |
| Metrics | [testillano/metrics](https://github.com/testillano/metrics) |
| Logger | [testillano/logger](https://github.com/testillano/logger) |

## Build with Docker

```bash
$ ./build.sh --image   # builds runtime image (ghcr.io/testillano/h2diagent:latest)
$ ./build.sh --builder # builds only deps stage
$ ./build.sh --ut      # builds unit test image
```

Or directly with Docker:

```bash
$ docker build --target runtime   -t h2diagent .
$ docker build --target deps      -t h2diagent_builder .
$ docker build --target unit-test -t h2diagent_ut .
```

### Building against a local (unpublished) diametercomm / diametercodec

By default the `deps` stage downloads the released `diametercomm` and
`diametercodec` (`ert_diametercomm_ver` / `ert_diametercodec_ver` in the
`Dockerfile`). To iterate on **unpublished** library changes (e.g. before
pushing/tagging a new version), inject your local working trees into the build:

```bash
# From the h2diagent repo root. Re-run whenever a library changes.
$ for lib in diametercomm diametercodec; do
    rsync -a --delete \
      --exclude='.git' --exclude='build' --exclude='CMakeCache.txt' --exclude='CMakeFiles' \
      /path/to/testillano_${lib}.master/ deps-local/${lib}/
  done

$ ./build.sh --image   # log shows ">>> Using LOCAL diametercomm source" (and diametercodec)
```

If `deps-local/<lib>/CMakeLists.txt` is present the `deps` stage builds that
source instead of the released tarball; otherwise it falls back to the released
version, so canonical/CI builds are unaffected. `deps-local/` is git-ignored.
Once your change is published, remove `deps-local/<lib>` and bump the version.

## Unit tests

```bash
$ ./ut.sh
```

## Execution

```bash
$ docker run --rm -it -p 3868:3868 -p 8074:8074 -p 8080:8080 ghcr.io/testillano/h2diagent:latest \
    --origin-host "mock-pcrf.example.com" \
    --origin-realm "example.com" \
    --dictionary /config/dictionary.json \
    --h2agent-host h2agent \
    --h2agent-port 8000 \
    --verbose
```

## Command-line options

```
h2diagent - C++ HTTP/2 - DIAMETER Gateway Service (translation agent)

Diameter:
  --diameter-port <port>          Diameter listen port (default: 3868)
  --diameter-peer-host <host>     Remote Diameter peer host (for outbound)
  --diameter-peer-port <port>     Remote Diameter peer port (default: 3868)
  --origin-host <identity>        Origin-Host for CER
  --origin-realm <realm>          Origin-Realm for CER
  --product-name <name>           Product-Name for CER (default: h2diagent)
  --dictionary <path>             Diameter dictionary JSON file path
  --watchdog-interval <seconds>   DWR interval (default: 30)
  --diameter-server-transport <tcp|sctp>  Inbound server (listener) transport;
                                  sctp is single-homing (default: tcp)
  --diameter-client-transport <tcp|sctp>  Outbound client (to peer) transport;
                                  sctp is single-homing (default: tcp)
  --diameter-connections <N>      Number of outbound Diameter client connections to
                                  the peer (default: 1). N>1 opens a round-robin pool.
                                  TCP only; with sctp, N>1 fails fast at startup.

Diameter TLS (TCP only; SCTP/DTLS not supported):
  --diameter-server-key <path>    Server private key (PEM) to enable inbound TLS
  --diameter-server-crt <path>    Server certificate (PEM) to enable inbound TLS
                                  (both key and crt required to secure the server)
  --diameter-server-key-password <pass>  Password for the server key (optional)
  --secure-diameter-client        Enable TLS on the outbound Diameter client
  --diameter-client-ca <path>     CA (PEM) to verify the remote server cert (optional)
  --diameter-client-crt <path>    Client certificate (PEM) for mutual TLS (optional)
  --diameter-client-key <path>    Client private key (PEM) for mutual TLS (optional)
  --diameter-client-key-password <pass>  Password for the client key (optional)

HTTP/2 (towards h2agent):
  --h2agent-host <host>           h2agent traffic server host (default: localhost)
  --h2agent-port <port>           h2agent traffic server port (default: 8000)

HTTP/2 (for outbound from h2agent):
  --http2-server-port <port>      HTTP/2 listen port for h2agent client (default: 8080)

General:
  --workers <n>                   Worker threads (default: nproc)
  --log-level <level>             Log level (default: Warning)
  --verbose                       Output log traces on console
  --admin-port <port>             Admin API port (default: 8074)
  --prometheus-port <port>        Prometheus scrape port (default: 9090)
  --disable-metrics               Disable prometheus metrics
  --version                       Program version
  --help                          This help
```

## Diameter client connection pool (`--diameter-connections`)

By default h2diagent opens a **single** outbound Diameter connection to the peer
(`--diameter-connections 1`), which is byte-for-byte the historical behaviour.
Set `--diameter-connections N` (N>1) to open a **pool of N connections** to the
same peer; outbound requests are striped **round-robin** across the pool and each
answer returns on the connection that carries it (per-transaction correlation is
kept per-connection, unchanged). Inbound server-initiated requests (e.g. RAR) are
answered back on the exact connection they arrived on.

Why use it (it is opt-in, not a throughput booster by default):

- **Robustness / graceful degradation.** With one connection, a peer reset or DPR
  stalls all Diameter traffic until reconnect. A pool keeps serving on the
  remaining N-1 connections while one reconnects (each connection reconnects
  independently). The gateway prefers a ready connection when sending.
- **Headroom** for a much faster SUT, where a single socket / correlation mutex
  could serialise before the network does.

Scope and prerequisites:

- **TCP only.** With `--diameter-client-transport sctp` and N>1 the process
  **fails fast at startup** with a clear message. SCTP uses a single one-to-one
  association here and multi-streaming (the conceptually-correct SCTP lever) is
  not implemented in `diametercomm`; the flag stays transport-agnostic in name so
  the restriction can be lifted later without a rename. Use N=1 for SCTP.
- **Peer must accept N connections per origin-host.** Each pooled connection does
  its own CER from the same Origin-Host; some peers cap connections per
  origin-host. Confirm your SUT accepts N before raising it.

## Secure Diameter (TLS/TCP)

h2diagent can secure the **Diameter** side with **TLS over TCP** (RFC 6733),
mirroring how `h2agent` secures its HTTP/2 interfaces. The HTTP/2 interface
towards `h2agent` is intentionally left in clear text (that hop is internal to
the mock; secure the Diameter hop that faces the SUT).

Both roles can be secured independently:

- **Diameter server (inbound).** TLS is enabled when **both** a key and a
  certificate are provided (same rule as h2agent's traffic server). The server
  presents its certificate (server authentication):

  ```bash
  h2diagent \
    --diameter-server-crt /ssl/server.crt \
    --diameter-server-key /ssl/server.key \
    --diameter-server-key-password "1111" \
    ...
  ```

- **Diameter client (outbound).** TLS is enabled with a flag (same idea as
  h2agent's client `"secure": true`). Optionally verify the remote server with a
  CA, and/or present a client certificate for mutual TLS (mTLS):

  ```bash
  h2diagent \
    --secure-diameter-client \
    --diameter-client-ca /ssl/ca.crt \
    [--diameter-client-crt /ssl/client.crt --diameter-client-key /ssl/client.key] \
    ...
  ```

  Without `--diameter-client-ca` the handshake still encrypts the channel but
  does not authenticate the peer (equivalent to h2agent's plain `secure`).

The startup log shows the effective mode per role, e.g.
`Diameter server listening on port 3868 [TLS]` /
`Diameter client connecting to sut:3868 [plain]`.

### Generating test certificates

`tools/ssl/create-certs.sh` generates a throwaway CA plus CA-signed server and
client certificates (PEM):

```bash
./tools/ssl/create-certs.sh localhost           # keys without password
./tools/ssl/create-certs.sh localhost "1111"    # password-protected keys
# -> ca.crt server.crt server.key client.crt client.key
```

### Gap: SCTP (DTLS/SCTP) is not supported

TLS is implemented for the **TCP** transport only. Diameter over **SCTP** would
be secured with **DTLS/SCTP** (RFC 6083), which is **not implemented**: the TLS
options above are **silently ignored** when the corresponding role uses
`--diameter-*-transport sctp` (the connection stays clear text). This is a known
limitation -- the underlying `boost::asio` transport used by `diametercomm` has
no DTLS support, and DTLS/SCTP requires an OpenSSL SCTP BIO outside that
abstraction. Use `tcp` transport to secure the Diameter side.

#### Can a socat/stunnel sidecar add DTLS/SCTP? No

A TLS/DTLS proxy sidecar does **not** close this gap:

- **socat** (checked with 1.7.4) exposes `openssl-dtls-*` (DTLS, but over
  **UDP** only) and `sctp-*` (plain SCTP, with **no** crypto). There is **no**
  address type that combines DTLS with SCTP, so socat cannot speak RFC 6083
  DTLS/SCTP.
- **stunnel** secures TCP only (no SCTP, no DTLS/SCTP).

Tunnelling the Diameter/SCTP bytes through a socat DTLS/**UDP** tunnel would
change the transport (UDP, not SCTP) and would **not interoperate** with a peer
expecting DTLS/SCTP -- a lab curiosity, not a real solution.

Practical ways to secure the Diameter hop:

1. **TCP + native TLS** (recommended) -- the options documented above.
2. **Secure SCTP at the network layer with IPsec** (transport/tunnel mode),
   which is transport-agnostic and needs no application-level DTLS.

For reference, securing a *TCP* Diameter hop with an external socat terminator
(equivalent to the built-in TLS; useful only if you must terminate TLS outside
the process, e.g. to front a plaintext peer) looks like:

```bash
# Server side: expose TLS on :3869 in front of a plaintext Diameter TCP :3868
socat OPENSSL-LISTEN:3869,reuseaddr,fork,cert=server.pem,cafile=ca.crt,verify=0 \
      TCP:localhost:3868

# Client side: accept plaintext on :3868, dial the remote TLS terminator
socat TCP-LISTEN:3868,reuseaddr,fork \
      OPENSSL:sut:3869,cafile=ca.crt,verify=1
```

There is deliberately no such one-liner for SCTP -- use IPsec or TCP+TLS instead.

## Peers (quick Diameter endpoint setup)

The `create-peer.sh` tool generates self-contained Diameter peers without needing to understand the internal architecture. Each peer is a directory under `peers/` with everything needed to run.

### Create a peer

```bash
# Interactive:
./tools/create-peer.sh

# Non-interactive (default output: ./peers/):
./tools/create-peer.sh -n pcrf-mock -s "gx rx" -r server --server-port 3868 --admin-port 8274

# Custom output dir (e.g., for git-tracked examples):
./tools/create-peer.sh -n pgw-sim -s gx -r client --peer-host localhost --peer-port 3868 -o ./examples/peers
```

### Run peers

```bash
source examples/peers/pcrf-mock/run.bash    # starts Diameter server (PCRF)
source examples/peers/pgw-sim/run.bash      # starts Diameter client (PGW), connects to PCRF
```

### Stop peers

```bash
source examples/peers/pcrf-mock/stop.bash
source examples/peers/pgw-sim/stop.bash
```

### Peer directory structure

```
peers/<name>/
  config.env              # generated configuration variables
  docker-compose.yml      # opaque: h2diagent + h2agent sidecar
  stacks/                 # Diameter dictionaries (from tools/stacks/)
    gx.json
    rx.json
  programming/            # h2agent provisions (mock behavior)
    server-matching.json
    server-provision.json
  run.bash                # start the peer
  stop.bash               # stop the peer
```

### Multi-stack support

A single peer can handle multiple Diameter applications. Use `--dictionary` multiple times (or space-separated stacks in `create-peer.sh`):

```bash
./tools/create-peer.sh -n pcrf-mock -s "gx rx sy" -r server
```

All loaded Application-IDs are advertised in the CER/CEA and each message is decoded/encoded with the correct dictionary based on its Application-ID header.

### Available stacks

| Stack | Application-ID | Interface |
|-------|---------------|-----------|
| gx | 16777238 | Policy and Charging Control (Gx) |
| rx | 16777236 | Policy Control over Rx |
| sy | 16777302 | Spending Limit (Sy) |

Custom stacks can be added to `tools/stacks/` or directly into `peers/<name>/stacks/`.

## Metrics and Monitoring

h2diagent exposes Prometheus metrics on port `8085` (configurable via `--prometheus-port`).

Combined with h2agent's metrics (port `8080`), you get full visibility of both the Diameter and HTTP/2 sides.

### Quick Prometheus setup

Use h2agent's [`prometheus-only.sh`](https://github.com/testillano/h2agent/blob/master/tools/grafana/prometheus-only.sh) script to quickly spin up a Prometheus instance that scrapes both:

```bash
# From h2agent checkout:
./tools/grafana/prometheus-only.sh 8080 8085   # h2agent:8080 + h2diagent:8085
```

### Available metrics

Metrics are exposed in two groups: **Diameter** (from `diametercomm` library via `enableMetrics()`) and **HTTP/2** (instrumented at Gateway level).

The label '**source**' corresponds to the `--product-name` command-line parameter (default: `h2diagent`).

#### Diameter server (inbound)

```
Counters provided by diametercomm library:

   diameter_server_requests_received_counter [source] [command_code]
   diameter_server_answers_sent_counter [source] [command_code] [result_code]
   diameter_server_peer_connections_counter [source] [state: open/closed]

   Bidirectional Diameter (RFC 6733) -- server-initiated requests sent on the
   server leg and the correlated answers received back. When h2diagent
   simulates a peer -- it may PUSH a request (e.g. RAR) down a
   connected peer and correlate the incoming answer (RAA) by hop-by-hop:

   diameter_server_requests_sent_counter [source] [command_code] [application_id]
   diameter_server_answers_received_counter [source] [command_code] [application_id] [result_code]
   diameter_server_answers_uncorrelated_counter [source] [command_code] [application_id]
   diameter_server_answers_recovered_counter [source] [command_code] [application_id] [by] [path]

Gauges provided by diametercomm library:

   diameter_server_active_peers_gauge [source]
```

#### Diameter client (outbound)

```
Counters provided by diametercomm library:

   diameter_client_requests_sent_counter [source] [command_code] [application_id]
   diameter_client_answers_received_counter [source] [command_code] [application_id] [result_code]
   diameter_client_requests_timedout_counter [source] [command_code] [application_id]
   diameter_client_requests_unsent_counter [source] [command_code] [application_id]
   diameter_client_answers_uncorrelated_counter [source] [command_code] [application_id]
      Answers dropped because no pending transaction matches on any connection/leg.
   diameter_client_answers_recovered_counter [source] [command_code] [application_id] [by: hbh/e2e] [path: same-leg/cross-leg]
      Answers correlated by end-to-end and/or received on another connection/leg.

   Bidirectional Diameter (RFC 6733) -- server-initiated requests received on
   the client leg and the answers h2diagent sends back on it. After CER/CEA the
   remote peer may push requests (e.g. RAR/DPR) on the connection h2diagent
   opened; they are forwarded to h2agent and answered on the same leg:

   diameter_client_requests_received_counter [source] [command_code] [application_id]
   diameter_client_answers_sent_counter [source] [command_code] [application_id] [result_code]

Gauges provided by diametercomm library:

   diameter_client_peer_state_gauge [source] (1=open, 0=closed)

Histograms provided by diametercomm library:

   diameter_client_response_delay_seconds [source] [command_code] [application_id]
      Round-trip latency (seconds) from request sent to correlated answer.
      Exposes _sum / _count / _bucket (used by metrics_summary for averages).
```

#### Metric labels: `application_id` and optional per-AVP labels

Every Diameter **client** metric always carries `source`, `command_code` and
**`application_id`** (the Application-Id read from the message header; together
with `command_code` it identifies the interface + command). Answers also carry
`result_code`.

You can attach **extra labels** whose value is taken, per outbound request, from
chosen AVPs (by name). The option is **repeatable**:

```bash
h2diagent --metrics-additional-label CC-Request-Type \
          --metrics-additional-label Termination-Cause ...
```

Each AVP name is sanitized into a Prometheus label key (e.g. `CC-Request-Type`
-> `cc_request_type`) and attached to all client metrics; the value is read from
the outbound request JSON and reused for the correlated answer. This lets you,
for example, break latency down by `CC-Request-Type` (initial/update/terminate)
without hard-coding Credit-Control semantics.

##### Typical AVPs worth labelling (low cardinality)

| AVP | Interface | Values | Note |
|-----|-----------|--------|------|
| `CC-Request-Type` | Gx/Gy/Ro | 1..4 (INITIAL/UPDATE/TERMINATION/EVENT) | the canonical breakdown |
| `Termination-Cause` | base (STR) | enumerated (~8) | session teardown reason |
| `Disconnect-Cause` | base (DPR) | 0/1/2 | peer disconnect reason |
| `Subscription-Id-Type` | Gx/Gy | 0..4 (E164/IMSI/SIP/NAI/...) | the **type**, not the data |
| `Auth-Application-Id` | generic | few | which application/interface |

> **The value is read from the request.** AVPs that only appear in the *answer*
> (e.g. an installed `QoS-Class-Identifier`) are not available with this
> mechanism.

> **Cardinality warning.** Prometheus degrades with high-cardinality labels. Use
> only **low-cardinality** (enumerated/type-like) AVPs. Do **NOT** use unbounded
> AVPs such as `Session-Id`, `Subscription-Id-Data` (IMSI/MSISDN),
> `Framed-IP-Address`, `CC-Request-Number` or `User-Name`: each distinct value
> creates a new time series and can overwhelm the TSDB.

#### HTTP/2 server (outbound triggers from h2agent)

```
Counters provided by h2diagent:

   h2diagent_http2_server_requests_received_counter [source] [method]
   h2diagent_http2_server_responses_sent_counter [source] [method] [status_code]
```

#### HTTP/2 client (inbound translation towards h2agent)

```
Counters provided by h2diagent:

   h2diagent_http2_client_requests_sent_counter [source] [method]
   h2diagent_http2_client_responses_received_counter [source] [method] [status_code]
```

#### Process resources

```
Provided by metrics library (ProcessCollector):

   process_cpu_seconds_total [source]
   process_resident_memory_bytes [source]
   process_virtual_memory_bytes [source]
   process_open_fds [source]
   process_max_fds [source]
   process_threads [source]
```

#### Examples

```bash
diameter_server_requests_received_counter{source="h2diagent",command_code="272"} 15
diameter_client_answers_received_counter{source="h2diagent",command_code="272",application_id="16777238",result_code="2001"} 15
h2diagent_http2_server_requests_received_counter{source="h2diagent",method="POST"} 15
h2diagent_http2_client_responses_received_counter{source="h2diagent",method="POST",status_code="200"} 15
diameter_server_active_peers_gauge{source="h2diagent"} 1
diameter_client_peer_state_gauge{source="h2diagent"} 1
```

### Helper functions (`tools/helpers.bash`)

Since h2diagent has no administrative REST interface, `tools/helpers.bash`
provides shortcut functions over the Prometheus metrics endpoint (h2diagent
only; the h2agent sidecar has its own helpers). Source it natively, or use it
inside the container (the helm chart mounts it and sources it into `~/.bashrc`
for `kubectl exec`):

```bash
source tools/helpers.bash   # native (targets localhost:8085 by default)

metrics [port]           # raw Prometheus metrics scraped from h2diagent
metrics_summary          # Diameter client summary (result-codes, PASS(2001),
                         # latency) with h2agent-like snapshot/delta support
```

`metrics_summary` mirrors h2agent's snapshot/delta command line. Diameter
counters only grow, so it works on **deltas** between two points in time (a
`zeroed` baseline yields absolute totals):

```bash
metrics_summary --now               # live view since start (zeroed -> now)
metrics_summary --save before       # mark a point
# ... run traffic ...
metrics_summary before --now        # delta from the mark to now
metrics_summary --delta             # incremental delta since the previous call
metrics_summary --show / --clean    # list / remove saved snapshots
metrics_summary before after --json # machine-readable delta
```

`metrics` takes an optional `[port]`; `metrics_summary` takes `--port <p>`
(both default to `H2DIAHLP_METRICS_PORT`). Use `-h` for full usage. Override
targets via `H2DIAHLP_METRICS_PORT`, `H2DIAHLP_SERVER_ADDR`, `H2DIAHLP_SCHEME`
(h2diagent scrapes with an inline `curl -s`, so there is no CURL override).

### Grafana dashboard

A Grafana dashboard JSON is provided at `tools/grafana/h2diagent-dashboard.json`.
Import it into your Grafana instance and configure the Prometheus datasource. See `tools/grafana/README.md` for details.

## Related projects

- [h2agent](https://github.com/testillano/h2agent) - HTTP/2 mock service (the brain)
- [diametercodec](https://github.com/testillano/diametercodec) - Diameter codec library
- [diametercomm](https://github.com/testillano/diametercomm) - Diameter communications library

## Contributing

Check the project [contributing guidelines](./CONTRIBUTING.md).

## License

[MIT](LICENSE)
