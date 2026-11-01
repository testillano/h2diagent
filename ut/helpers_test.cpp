/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for helper functions.
*/

#include <gtest/gtest.h>

#include <atomic>
#include <ert/h2diagent/helpers.hpp>

using namespace ert::h2diagent::helpers;

// =============================================================================
// appIdToInterface
// =============================================================================
TEST(Helpers, AppIdToInterface_Gx) { EXPECT_EQ(appIdToInterface(16777238), "gx"); }
TEST(Helpers, AppIdToInterface_Rx) { EXPECT_EQ(appIdToInterface(16777236), "rx"); }
TEST(Helpers, AppIdToInterface_Sy) { EXPECT_EQ(appIdToInterface(16777302), "sy"); }
TEST(Helpers, AppIdToInterface_Cc) { EXPECT_EQ(appIdToInterface(4), "cc"); }
TEST(Helpers, AppIdToInterface_Unknown) { EXPECT_EQ(appIdToInterface(99999), "app-99999"); }

// =============================================================================
// commandCodeToName
// =============================================================================
TEST(Helpers, CommandCodeToName_CCR) { EXPECT_EQ(commandCodeToName(272), "CCR"); }
TEST(Helpers, CommandCodeToName_AAR) { EXPECT_EQ(commandCodeToName(265), "AAR"); }
TEST(Helpers, CommandCodeToName_RAR) { EXPECT_EQ(commandCodeToName(258), "RAR"); }
TEST(Helpers, CommandCodeToName_DWR) { EXPECT_EQ(commandCodeToName(280), "DWR"); }
TEST(Helpers, CommandCodeToName_CER) { EXPECT_EQ(commandCodeToName(257), "CER"); }
TEST(Helpers, CommandCodeToName_Unknown) { EXPECT_EQ(commandCodeToName(999), "CMD-999"); }

TEST(Helpers, CommandCodeToAnswerName_CCA) { EXPECT_EQ(commandCodeToAnswerName(272), "CCA"); }
TEST(Helpers, CommandCodeToAnswerName_AAA) { EXPECT_EQ(commandCodeToAnswerName(265), "AAA"); }

// =============================================================================
// Binary header extraction
// =============================================================================
class HeaderExtraction : public ::testing::Test {
   protected:
    // Build a minimal 20-byte Diameter header:
    // [0]    version=1
    // [1-3]  message length (3 bytes)
    // [4]    flags (R-bit = 0x80)
    // [5-7]  command code (3 bytes)
    // [8-11] application-id (4 bytes)
    // [12-15] hop-by-hop (4 bytes)
    // [16-19] end-to-end (4 bytes)
    std::vector<uint8_t> buildHeader(uint8_t flags, uint32_t code, uint32_t appId, uint32_t hbh, uint32_t e2e) {
        std::vector<uint8_t> msg(20, 0);
        msg[0] = 1;  // version
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 20;  // length
        msg[4] = flags;
        msg[5] = (code >> 16) & 0xFF;
        msg[6] = (code >> 8) & 0xFF;
        msg[7] = code & 0xFF;
        msg[8] = (appId >> 24) & 0xFF;
        msg[9] = (appId >> 16) & 0xFF;
        msg[10] = (appId >> 8) & 0xFF;
        msg[11] = appId & 0xFF;
        msg[12] = (hbh >> 24) & 0xFF;
        msg[13] = (hbh >> 16) & 0xFF;
        msg[14] = (hbh >> 8) & 0xFF;
        msg[15] = hbh & 0xFF;
        msg[16] = (e2e >> 24) & 0xFF;
        msg[17] = (e2e >> 16) & 0xFF;
        msg[18] = (e2e >> 8) & 0xFF;
        msg[19] = e2e & 0xFF;
        return msg;
    }
};

TEST_F(HeaderExtraction, ExtractCommandCode) {
    auto msg = buildHeader(0x80, 272, 16777238, 1, 1);
    EXPECT_EQ(extractCommandCode(msg), 272u);
}

TEST_F(HeaderExtraction, ExtractApplicationId) {
    auto msg = buildHeader(0x80, 272, 16777238, 1, 1);
    EXPECT_EQ(extractApplicationId(msg), 16777238u);
}

TEST_F(HeaderExtraction, ExtractHopByHop) {
    auto msg = buildHeader(0x80, 272, 0, 0xDEADBEEF, 0);
    EXPECT_EQ(extractHopByHop(msg), 0xDEADBEEFu);
}

TEST_F(HeaderExtraction, ExtractEndToEnd) {
    auto msg = buildHeader(0x80, 272, 0, 0, 0xCAFEBABE);
    EXPECT_EQ(extractEndToEnd(msg), 0xCAFEBABEu);
}

TEST_F(HeaderExtraction, IsRequest_True) {
    auto msg = buildHeader(0x80, 272, 0, 0, 0);
    EXPECT_TRUE(isRequest(msg));
}

TEST_F(HeaderExtraction, IsRequest_False) {
    auto msg = buildHeader(0x00, 272, 0, 0, 0);
    EXPECT_FALSE(isRequest(msg));
}

TEST_F(HeaderExtraction, TooShort_ReturnsZero) {
    std::vector<uint8_t> msg(10, 0);
    EXPECT_EQ(extractCommandCode(msg), 0u);
    EXPECT_EQ(extractApplicationId(msg), 0u);
    EXPECT_EQ(extractHopByHop(msg), 0u);
    EXPECT_EQ(extractEndToEnd(msg), 0u);
    EXPECT_FALSE(isRequest(msg));
}

// =============================================================================
// parseUri
// =============================================================================
TEST(Helpers, ParseUri_Gx_CCR) {
    uint32_t appId = 0, code = 0;
    parseUri("/diameter/gx/CCR", appId, code);
    EXPECT_EQ(appId, 16777238u);
    EXPECT_EQ(code, 272u);
}

TEST(Helpers, ParseUri_Rx_AAR) {
    uint32_t appId = 0, code = 0;
    parseUri("/diameter/rx/AAR", appId, code);
    EXPECT_EQ(appId, 16777236u);
    EXPECT_EQ(code, 265u);
}

TEST(Helpers, ParseUri_Sy_SNR) {
    uint32_t appId = 0, code = 0;
    parseUri("/diameter/sy/SNR", appId, code);
    EXPECT_EQ(appId, 16777302u);
    EXPECT_EQ(code, 300u);
}

TEST(Helpers, ParseUri_Unknown) {
    uint32_t appId = 0, code = 0;
    parseUri("/diameter/unknown/XYZ", appId, code);
    EXPECT_EQ(appId, 0u);
    EXPECT_EQ(code, 0u);
}

TEST(Helpers, ParseUri_Invalid) {
    uint32_t appId = 0, code = 0;
    parseUri("/invalid", appId, code);
    EXPECT_EQ(appId, 0u);
    EXPECT_EQ(code, 0u);
}

// =============================================================================
// normalizeTransport (CLI --diameter-*-transport parsing)
// =============================================================================
TEST(Helpers, NormalizeTransport_Tcp) {
    std::string norm;
    EXPECT_TRUE(normalizeTransport("tcp", norm));
    EXPECT_EQ(norm, "tcp");
}

TEST(Helpers, NormalizeTransport_Sctp) {
    std::string norm;
    EXPECT_TRUE(normalizeTransport("sctp", norm));
    EXPECT_EQ(norm, "sctp");
}

TEST(Helpers, NormalizeTransport_CaseInsensitive) {
    std::string norm;
    EXPECT_TRUE(normalizeTransport("TCP", norm));
    EXPECT_EQ(norm, "tcp");
    EXPECT_TRUE(normalizeTransport("ScTp", norm));
    EXPECT_EQ(norm, "sctp");
}

TEST(Helpers, NormalizeTransport_InvalidRejected) {
    std::string norm = "unchanged";
    EXPECT_FALSE(normalizeTransport("udp", norm));
    EXPECT_EQ(norm, "unchanged");  // left untouched on failure
    EXPECT_FALSE(normalizeTransport("", norm));
    EXPECT_FALSE(normalizeTransport("tcp ", norm));  // trailing space is not a valid token
    EXPECT_FALSE(normalizeTransport("sctpx", norm));
    EXPECT_EQ(norm, "unchanged");
}

// =============================================================================
// validateDiameterConnections (CLI --diameter-connections rule)
// =============================================================================
TEST(Helpers, ValidateDiameterConnections_DefaultOneTcp) {
    std::string err = "x";
    EXPECT_TRUE(validateDiameterConnections(1, /*isSctp=*/false, err));
    EXPECT_TRUE(err.empty());
}

TEST(Helpers, ValidateDiameterConnections_DefaultOneSctp) {
    // N=1 is today's behaviour; allowed for SCTP too.
    std::string err = "x";
    EXPECT_TRUE(validateDiameterConnections(1, /*isSctp=*/true, err));
    EXPECT_TRUE(err.empty());
}

TEST(Helpers, ValidateDiameterConnections_PoolTcpOk) {
    std::string err = "x";
    EXPECT_TRUE(validateDiameterConnections(4, /*isSctp=*/false, err));
    EXPECT_TRUE(err.empty());
}

TEST(Helpers, ValidateDiameterConnections_PoolSctpRejected) {
    // SCTP + N>1 must fail fast (multi-streaming not implemented).
    std::string err;
    EXPECT_FALSE(validateDiameterConnections(2, /*isSctp=*/true, err));
    EXPECT_FALSE(err.empty());  // must carry a clear message
}

TEST(Helpers, ValidateDiameterConnections_ZeroOrNegativeRejected) {
    std::string err;
    EXPECT_FALSE(validateDiameterConnections(0, /*isSctp=*/false, err));
    EXPECT_FALSE(err.empty());
    err.clear();
    EXPECT_FALSE(validateDiameterConnections(-3, /*isSctp=*/false, err));
    EXPECT_FALSE(err.empty());
}

// =============================================================================
// roundRobinIndex (pool striping selection) -- pure, used by DiameterClientPool
// =============================================================================
TEST(Helpers, RoundRobinIndex_SingleConnection) {
    // Pool of 1: always index 0 (today's single-connection behaviour).
    std::atomic<std::size_t> cursor{0};
    for (int i = 0; i < 5; ++i) EXPECT_EQ(roundRobinIndex(cursor, 1), 0u);
}

TEST(Helpers, RoundRobinIndex_CyclesAcrossPool) {
    std::atomic<std::size_t> cursor{0};
    // N=3: 0,1,2,0,1,2 ...
    EXPECT_EQ(roundRobinIndex(cursor, 3), 0u);
    EXPECT_EQ(roundRobinIndex(cursor, 3), 1u);
    EXPECT_EQ(roundRobinIndex(cursor, 3), 2u);
    EXPECT_EQ(roundRobinIndex(cursor, 3), 0u);
    EXPECT_EQ(roundRobinIndex(cursor, 3), 1u);
}

TEST(Helpers, RoundRobinIndex_EvenDistribution) {
    std::atomic<std::size_t> cursor{0};
    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4000; ++i) counts[roundRobinIndex(cursor, 4)]++;
    for (int i = 0; i < 4; ++i) EXPECT_EQ(counts[i], 1000);
}

TEST(Helpers, RoundRobinIndex_ZeroSizeSafe) {
    // Defensive: size 0 must not divide-by-zero; returns 0.
    std::atomic<std::size_t> cursor{0};
    EXPECT_EQ(roundRobinIndex(cursor, 0), 0u);
}

// =============================================================================
// Raw message helpers (negative testing)
// =============================================================================
TEST(Helpers, HexToBytes_Valid) {
    std::vector<uint8_t> out;
    ASSERT_TRUE(hexToBytes("00aBfF", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0x00, 0xab, 0xff}));
    ASSERT_TRUE(hexToBytes("", out));
    EXPECT_TRUE(out.empty());
}

TEST(Helpers, HexToBytes_Invalid) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(hexToBytes("abc", out));  // odd
    EXPECT_FALSE(hexToBytes("zz", out));   // non-hex
    EXPECT_FALSE(hexToBytes("0x01", out));
    EXPECT_FALSE(hexToBytes("01 02", out));
}

TEST(Helpers, BytesToHex_RoundTrip) {
    std::vector<uint8_t> in{0x01, 0x00, 0xfe};
    EXPECT_EQ(bytesToHex(in), "0100fe");
    std::vector<uint8_t> back;
    ASSERT_TRUE(hexToBytes(bytesToHex(in), back));
    EXPECT_EQ(back, in);
}

TEST(Helpers, HexId) {
    EXPECT_EQ(hexId(0), "0x00000000");
    EXPECT_EQ(hexId(0xdeadBEEF), "0xdeadbeef");
}

// 20-byte header: version 1, length 24, R-bit, CCR (272), Gx, hbh/e2e given.
static std::string rawHeaderHex(const std::string& hbh, const std::string& e2e) {
    return "01000018"
           "80000110"
           "01000016" +
           hbh + e2e + "deadbeef";
}

TEST(Helpers, RawMessageFromHex_FillsZeroIds) {
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(rawMessageFromHex(rawHeaderHex("00000000", "00000000"), 0x11111111, 0x22222222, out, err));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(out.size(), 24u);
    EXPECT_EQ(extractHopByHop(out), 0x11111111u);
    EXPECT_EQ(extractEndToEnd(out), 0x22222222u);
    EXPECT_EQ(extractCommandCode(out), 272u);
    EXPECT_EQ(out[23], 0xef);  // body kept verbatim
}

TEST(Helpers, RawMessageFromHex_KeepsNonZeroIdsAndZeroFill) {
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_TRUE(rawMessageFromHex(rawHeaderHex("000000aa", "000000bb"), 0x11111111, 0x22222222, out, err));
    EXPECT_EQ(extractHopByHop(out), 0xaau);
    EXPECT_EQ(extractEndToEnd(out), 0xbbu);
    // Request case: fill values 0 keep zero ids (the Diameter client assigns them).
    ASSERT_TRUE(rawMessageFromHex(rawHeaderHex("00000000", "00000000"), 0, 0, out, err));
    EXPECT_EQ(extractHopByHop(out), 0u);
    EXPECT_EQ(extractEndToEnd(out), 0u);
}

TEST(Helpers, RawMessageFromHex_Rejects) {
    std::vector<uint8_t> out;
    std::string err;
    EXPECT_FALSE(rawMessageFromHex("010000", 0, 0, out, err));  // < 20 bytes
    EXPECT_NE(err.find("20-byte"), std::string::npos);
    EXPECT_FALSE(rawMessageFromHex("0g", 0, 0, out, err));
    EXPECT_NE(err.find("hex"), std::string::npos);
    EXPECT_FALSE(rawMessageFromHex(std::string(39, '0'), 0, 0, out, err));  // odd
}

TEST(Helpers, HeaderJsonFromRaw) {
    std::vector<uint8_t> msg;
    ASSERT_TRUE(hexToBytes(rawHeaderHex("00000001", "00000002"), msg));
    auto h = headerJsonFromRaw(msg);
    EXPECT_EQ(h["version"], 1);
    EXPECT_EQ(h["flags"], 0x80);
    EXPECT_EQ(h["command-code"], 272);
    EXPECT_EQ(h["request"], true);
    EXPECT_EQ(h["application-id"], 16777238);
    EXPECT_EQ(h["hop-by-hop-id"], 1);
    EXPECT_EQ(h["end-to-end-id"], 2);
    EXPECT_TRUE(headerJsonFromRaw(std::vector<uint8_t>(10, 0)).empty());
}

TEST(Helpers, UndecodedMessageJson) {
    std::vector<uint8_t> msg;
    ASSERT_TRUE(hexToBytes(rawHeaderHex("00000001", "00000002"), msg));
    auto j = undecodedMessageJson(msg, "AVP length smaller than header");
    EXPECT_EQ(j["_raw"], rawHeaderHex("00000001", "00000002"));
    EXPECT_EQ(j["_decodeError"], "AVP length smaller than header");
    EXPECT_EQ(j["_header"]["command-code"], 272);
}

TEST(Helpers, ErrorBody_IsValidJsonWithQuotes) {
    std::string body = errorBody("Invalid JSON: unexpected '\"' at \\ end");
    auto j = nlohmann::json::parse(body);  // must not throw
    EXPECT_EQ(j["error"], "Invalid JSON: unexpected '\"' at \\ end");
}

TEST(Helpers, ErrorBody_MergesExtra) {
    auto j =
        nlohmann::json::parse(errorBody("Diameter answer timeout", {{"hopByHop", "0x00000001"}, {"timeoutMs", 5}}));
    EXPECT_EQ(j["error"], "Diameter answer timeout");
    EXPECT_EQ(j["hopByHop"], "0x00000001");
    EXPECT_EQ(j["timeoutMs"], 5);
    // 'error' always wins; non-object extra ignored
    EXPECT_EQ(nlohmann::json::parse(errorBody("e", {{"error", "x"}}))["error"], "e");
    EXPECT_EQ(nlohmann::json::parse(errorBody("e", nlohmann::json::array()))["error"], "e");
}
