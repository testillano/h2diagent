/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for helper functions.
*/

#include <gtest/gtest.h>

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
