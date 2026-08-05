# Diameter AVP representation in JSON

This document describes how each Diameter AVP **data format** is represented in
the JSON that h2diagent exchanges with h2agent:

- **client-provision** requests (h2agent -> h2diagent -> Diameter peer), and
- **server-provision** responses (Diameter peer -> h2diagent -> h2agent).

## Model: representation is driven by the AVP format in the dictionary

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

## Per-format table (the 16 base/derived formats)

| Diameter format   | JSON type | Example (JSON)                         | On-the-wire meaning |
|-------------------|-----------|----------------------------------------|---------------------|
| Integer32         | number    | `"CC-Request-Number": 0`               | signed 32-bit |
| Integer64         | number    | `"Accounting-Sub-Session-Id": 12345`   | signed 64-bit |
| Unsigned32        | number    | `"Auth-Application-Id": 16777238`      | unsigned 32-bit |
| Unsigned64        | number    | `"CC-Total-Octets": 1099511627776`     | unsigned 64-bit |
| Float32           | number    | `"Some-Float32": 1.5`                  | IEEE 754 single |
| Float64           | number    | `"Some-Float64": 1.5`                  | IEEE 754 double |
| Enumerated        | number    | `"CC-Request-Type": 1`                 | signed 32-bit (enum value; no alias in flat JSON) |
| OctetString       | string (hex) | `"Framed-IP-Address": "c0a80001"`   | raw bytes; `"af01"` -> 0xAF 0x01 |
| UTF8String        | string    | `"Subscription-Id-Data": "1234567890"` | UTF-8 text |
| DiameterIdentity  | string    | `"Origin-Host": "pcef.example.com"`    | text |
| DiameterURI       | string    | `"Redirect-Server-Address": "aaa://host:3868"` | text |
| IPFilterRule      | string    | `"IPFilterRule-Avp": "permit in ip from any to any"` | text |
| QoSFilterRule     | string    | `"QoSFilterRule-Avp": "..."`           | text |
| Time              | number    | `"Event-Timestamp": 1700000000`        | UNIX epoch seconds (engine converts to/from NTP) |
| Address           | string    | `"Some-Address": "192.168.0.2"`        | IPv4/IPv6 human form; engine encodes family+bytes on the wire |
| Grouped           | object    | see below                              | nested AVPs |

### OctetString (hex)

OctetString carries opaque bytes, so it is written as a **hex string** (even
length). Examples:
```json
"Framed-IP-Address": "c0a80001",          // 4 raw bytes = 192.168.0.1
"3GPP-User-Location-Info": "8064f0000064" // packed binary, verbatim bytes
```
`"af01"` decodes to the two bytes 0xAF 0x01. The hex must be valid and even
length; invalid hex is an error (do not rely on silent parsing).

IMPORTANT for IP-bearing OctetStrings: many 3GPP AVPs (e.g. `Framed-IP-Address`,
AVP code 8) are declared **OctetString** and carry the **4 raw bytes** of the
IPv4 address per the norm. Therefore they must be written as hex of those 4
bytes (192.168.0.2 -> `"c0a80002"`), NOT as the dotted-decimal string
`"192.168.0.2"` and NOT as the ASCII of the dotted string. Writing a
dotted-decimal string into an OctetString produces wrong bytes and the peer will
reject the message (e.g. Result-Code 5004 DIAMETER_INVALID_AVP_VALUE).

### Grouped

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

### Time

Represented as **UNIX epoch seconds** (a plain number). The engine converts
to/from the Diameter NTP timestamp (adds/subtracts the NTP epoch offset).

## Caveats / notes (engine)

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

## Debug traces (per-AVP format mapping)

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

## Summary

Write each AVP value in the canonical representation for its dictionary format:
numbers for integer/unsigned/float/enum, human text for the text formats, UNIX
epoch for Time, hex for OctetString, and nested objects/arrays for Grouped. The
engine encodes to the normative Diameter wire bytes according to that format.
