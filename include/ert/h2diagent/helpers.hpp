/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ert/http2comm/URLFunctions.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ert {
namespace h2diagent {
namespace helpers {

// Application-ID to interface name mapping
inline std::string appIdToInterface(uint32_t appId) {
    switch (appId) {
        case 16777238:
            return "gx";
        case 16777236:
            return "rx";
        case 16777302:
            return "sy";
        case 4:
            return "cc";
        default:
            return "app-" + std::to_string(appId);
    }
}

// Command-code to abbreviation (request form)
inline std::string commandCodeToName(uint32_t code) {
    switch (code) {
        case 257:
            return "CER";
        case 258:
            return "RAR";
        case 265:
            return "AAR";
        case 271:
            return "ACR";
        case 272:
            return "CCR";
        case 274:
            return "ASR";
        case 275:
            return "STR";
        case 280:
            return "DWR";
        case 282:
            return "DPR";
        case 300:
            return "SNR";
        default:
            return "CMD-" + std::to_string(code);
    }
}

// Command name for answer (last char -> 'A')
inline std::string commandCodeToAnswerName(uint32_t code) {
    std::string name = commandCodeToName(code);
    if (!name.empty()) name.back() = 'A';
    return name;
}

// Extract command code from Diameter binary header (bytes 5-7)
inline uint32_t extractCommandCode(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[5]) << 16) | (uint32_t(msg[6]) << 8) | uint32_t(msg[7]);
}

// Extract application-id from Diameter binary header (bytes 8-11)
inline uint32_t extractApplicationId(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[8]) << 24) | (uint32_t(msg[9]) << 16) | (uint32_t(msg[10]) << 8) | uint32_t(msg[11]);
}

// Extract hop-by-hop from Diameter binary header (bytes 12-15)
inline uint32_t extractHopByHop(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) | (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

// Extract end-to-end from Diameter binary header (bytes 16-19)
inline uint32_t extractEndToEnd(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[16]) << 24) | (uint32_t(msg[17]) << 16) | (uint32_t(msg[18]) << 8) | uint32_t(msg[19]);
}

// Check if request (R-bit set in flags byte 4)
inline bool isRequest(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return false;
    return (msg[4] & 0x80) != 0;
}

// Session-Id (AVP 263, base protocol, no Vendor-Id) read raw from the wire: no
// dictionary, no codec, so it is available even when the message cannot be
// decoded. RFC 6733 places it first (< Session-Id >), so the loop normally
// stops at the first AVP; a misplaced one is still found by hopping AVP headers
// (no AVP data is parsed). The value is returned byte-for-byte (UTF8String is
// not validated: it is used as a key). Returns "" when absent, or when the
// framing up to it is broken (AVP length shorter than its header, or beyond the
// message length / buffer).
inline std::string extractSessionId(const std::vector<uint8_t>& msg) {
    constexpr uint32_t kSessionIdCode = 263;
    constexpr uint8_t kVendorBit = 0x80;
    constexpr size_t kMessageHeaderLen = 20;
    constexpr size_t kAvpHeaderLen = 8;
    constexpr size_t kVendorAvpHeaderLen = 12;

    if (msg.size() < kMessageHeaderLen) return "";
    const size_t msgLen = (size_t(msg[1]) << 16) | (size_t(msg[2]) << 8) | size_t(msg[3]);
    const size_t end = std::min(msg.size(), msgLen);
    size_t pos = kMessageHeaderLen;
    while (pos + kAvpHeaderLen <= end) {
        const uint32_t code = (uint32_t(msg[pos]) << 24) | (uint32_t(msg[pos + 1]) << 16) |
                              (uint32_t(msg[pos + 2]) << 8) | uint32_t(msg[pos + 3]);
        const uint8_t flags = msg[pos + 4];
        const size_t len = (size_t(msg[pos + 5]) << 16) | (size_t(msg[pos + 6]) << 8) | size_t(msg[pos + 7]);
        const bool vendor = (flags & kVendorBit) != 0;
        const size_t hdrLen = vendor ? kVendorAvpHeaderLen : kAvpHeaderLen;
        if (len < hdrLen || pos + len > end) return "";  // broken framing
        if (code == kSessionIdCode && !vendor) return std::string(msg.begin() + pos + hdrLen, msg.begin() + pos + len);
        pos += (len + 3) & ~size_t(3);  // AVPs are padded to 4 bytes
    }
    return "";
}

// URI path of the HTTP/2 POST forwarded to h2agent for an inbound Diameter
// request: /diameter/<interface>/<command>[/<percent-encoded Session-Id>].
// The Session-Id makes the URI (and so the h2agent state machine and server
// data key) per session. Every byte outside RFC 3986 unreserved is
// percent-encoded (';', '/', '%', UTF-8...), so the segment is always URI-safe.
// No Session-Id: bare /diameter/<interface>/<command>.
inline std::string inboundRequestPath(const std::string& interface, const std::string& command,
                                      const std::string& sessionId) {
    std::string path = "/diameter/" + interface + "/" + command;
    if (!sessionId.empty()) path += "/" + ert::http2comm::URLFunctions::encode(sessionId);
    return path;
}

// Parse interface and command from URI (e.g., /diameter/gx/CCR -> appId=16777238, code=272).
// Anything after the command (e.g. /diameter/rx/AAR/<session-id>) is ignored.
inline void parseUri(const std::string& uri, uint32_t& appId, uint32_t& commandCode) {
    appId = 0;
    commandCode = 0;
    size_t firstSlash = uri.find('/', 1);
    if (firstSlash == std::string::npos) return;
    size_t secondSlash = uri.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) return;

    std::string iface = uri.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    size_t cmdEnd = uri.find_first_of("/?", secondSlash + 1);
    std::string cmd =
        uri.substr(secondSlash + 1, (cmdEnd == std::string::npos) ? std::string::npos : cmdEnd - secondSlash - 1);

    if (iface == "gx")
        appId = 16777238;
    else if (iface == "rx")
        appId = 16777236;
    else if (iface == "sy")
        appId = 16777302;
    else if (iface == "cc")
        appId = 4;

    if (cmd == "CCR" || cmd == "CCA")
        commandCode = 272;
    else if (cmd == "AAR" || cmd == "AAA")
        commandCode = 265;
    else if (cmd == "RAR" || cmd == "RAA")
        commandCode = 258;
    else if (cmd == "ASR" || cmd == "ASA")
        commandCode = 274;
    else if (cmd == "STR" || cmd == "STA")
        commandCode = 275;
    else if (cmd == "ACR" || cmd == "ACA")
        commandCode = 271;
    else if (cmd == "SNR" || cmd == "SNA")
        commandCode = 300;
}

// Normalize a Diameter transport CLI token. Accepts "tcp"/"sctp"
// case-insensitively. On success sets 'normalized' to the lowercase canonical
// form ("tcp" or "sctp") and returns true; on any other value returns false and
// leaves 'normalized' unchanged.
inline bool normalizeTransport(const std::string& value, std::string& normalized) {
    std::string v;
    v.reserve(value.size());
    for (char c : value) v.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    if (v == "tcp" || v == "sctp") {
        normalized = v;
        return true;
    }
    return false;
}

// Validate the --diameter-connections value against the client transport.
// Rules (see the connection-pool proposal):
//   - N must be >= 1.
//   - N == 1 is today's single-connection behaviour, allowed for any transport.
//   - N  > 1 opens a client pool, supported over TCP ONLY. Over SCTP it FAILS
//     FAST (the SCTP association is single/one-to-one and multi-streaming is not
//     implemented; the conceptually-correct SCTP lever is multi-streaming, a
//     future diametercomm capability). The flag stays transport-agnostic in name;
//     the scope is enforced here in behaviour.
// On failure returns false and fills 'err' with a clear message; on success
// returns true and clears 'err'.
inline bool validateDiameterConnections(int connections, bool isSctp, std::string& err) {
    if (connections < 1) {
        err = "--diameter-connections must be >= 1";
        return false;
    }
    if (connections > 1 && isSctp) {
        err =
            "--diameter-connections > 1 is only supported over TCP; SCTP uses a single association "
            "(multi-streaming not implemented). Use N=1 for SCTP.";
        return false;
    }
    err.clear();
    return true;
}

// Round-robin index selection for the Diameter client connection pool. Returns
// the connection index to use for the next outbound request and advances the
// cursor by one (one relaxed atomic increment). 'size' is the pool size.
// Defensive: size 0 returns 0 (never divides by zero).
inline std::size_t roundRobinIndex(std::atomic<std::size_t>& cursor, std::size_t size) {
    if (size <= 1) return 0;
    return cursor.fetch_add(1, std::memory_order_relaxed) % size;
}

// =============================================================================
// Raw message helpers (negative testing: '_raw' JSON key, decode failures)
// =============================================================================

// Strict hex -> bytes: even length, hex digits only (either case). Returns
// false (and leaves 'out' unspecified) on invalid input.
inline bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Bytes -> lowercase hex.
inline std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

inline void setHopByHop(std::vector<uint8_t>& msg, uint32_t hbh) {
    if (msg.size() < 20) return;
    msg[12] = static_cast<uint8_t>(hbh >> 24);
    msg[13] = static_cast<uint8_t>(hbh >> 16);
    msg[14] = static_cast<uint8_t>(hbh >> 8);
    msg[15] = static_cast<uint8_t>(hbh);
}

inline void setEndToEnd(std::vector<uint8_t>& msg, uint32_t e2e) {
    if (msg.size() < 20) return;
    msg[16] = static_cast<uint8_t>(e2e >> 24);
    msg[17] = static_cast<uint8_t>(e2e >> 16);
    msg[18] = static_cast<uint8_t>(e2e >> 8);
    msg[19] = static_cast<uint8_t>(e2e);
}

// Build a raw Diameter message from the '_raw' hex value. The bytes are kept
// verbatim (length, flags, AVPs: whatever the test wants) except hop-by-hop /
// end-to-end, which are filled with 'hbhIfZero' / 'e2eIfZero' when they are 0
// so correlation keeps working (an answer gets the request ids; a request
// passes 0 and lets the Diameter client assign them). At least the 20-byte
// header is required. On failure returns false with a reason in 'err'.
inline bool rawMessageFromHex(const std::string& hex, uint32_t hbhIfZero, uint32_t e2eIfZero, std::vector<uint8_t>& out,
                              std::string& err) {
    if (!hexToBytes(hex, out)) {
        err = "'_raw' must be an even-length hex string";
        return false;
    }
    if (out.size() < 20) {
        err = "'_raw' must contain at least the 20-byte Diameter header";
        return false;
    }
    if (extractHopByHop(out) == 0 && hbhIfZero != 0) setHopByHop(out, hbhIfZero);
    if (extractEndToEnd(out) == 0 && e2eIfZero != 0) setEndToEnd(out, e2eIfZero);
    err.clear();
    return true;
}

// '_header' JSON read from the first 20 bytes (same fields as the codec emits),
// for messages that could not be decoded.
inline nlohmann::json headerJsonFromRaw(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return nlohmann::json::object();
    return {{"version", msg[0]},
            {"flags", msg[4]},
            {"command-code", extractCommandCode(msg)},
            {"request", isRequest(msg)},
            {"application-id", extractApplicationId(msg)},
            {"hop-by-hop-id", extractHopByHop(msg)},
            {"end-to-end-id", extractEndToEnd(msg)}};
}

// JSON body sent to h2agent when an inbound Diameter message cannot be decoded:
// the raw bytes, the decoder error and the header (so provisions can still
// route/answer it, e.g. with 5014 via '_raw' or plain JSON).
inline nlohmann::json undecodedMessageJson(const std::vector<uint8_t>& msg, const std::string& decodeError) {
    return {{"_raw", bytesToHex(msg)}, {"_decodeError", decodeError}, {"_header", headerJsonFromRaw(msg)}};
}

// Error body for HTTP/2 responses: valid JSON whatever the message contains
// (e.g. quotes in an exception text). 'extra' fields are merged in.
inline std::string errorBody(const std::string& error, const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["error"] = error;
    return j.dump();
}

// "0x%08x" rendering of a hop-by-hop / end-to-end id.
inline std::string hexId(uint32_t id) {
    static const char* digits = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(id >> shift) & 0xf]);
    return out;
}

}  // namespace helpers
}  // namespace h2diagent
}  // namespace ert
