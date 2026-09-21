/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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

// Parse interface and command from URI (e.g., /diameter/gx/CCR -> appId=16777238, code=272)
inline void parseUri(const std::string& uri, uint32_t& appId, uint32_t& commandCode) {
    appId = 0;
    commandCode = 0;
    size_t firstSlash = uri.find('/', 1);
    if (firstSlash == std::string::npos) return;
    size_t secondSlash = uri.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) return;

    std::string iface = uri.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    std::string cmd = uri.substr(secondSlash + 1);

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

}  // namespace helpers
}  // namespace h2diagent
}  // namespace ert
