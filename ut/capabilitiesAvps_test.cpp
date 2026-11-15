/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for the extra CER / CEA AVPs (--cer / --cea, CapabilitiesAvps.hpp).
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <ert/diametercodec/json/stacks.hpp>
#include <ert/h2diagent/CapabilitiesAvps.hpp>
#include <nlohmann/json.hpp>

using ert::h2diagent::helpers::CapabilitiesBuilt;
using ert::h2diagent::helpers::encodeCapabilitiesAvps;

namespace {
// The base protocol stack (embedded in diametercodec).
const ert::diametercodec::stack::Dictionary& base() {
    static ert::diametercodec::stack::Dictionary dict;
    static bool loaded = false;
    if (!loaded) {
        dict.load(ert::diametercodec::json::stacks::base);
        loaded = true;
    }
    return dict;
}

const CapabilitiesBuilt Built{"gw.example.com", "example.com", "h2diagent", 0};

// Codes of the encoded AVPs, in order.
std::vector<uint32_t> codes(const std::vector<uint8_t>& avps) {
    std::vector<uint32_t> result;
    std::size_t pos = 0;
    while (pos + 8 <= avps.size()) {
        result.push_back((uint32_t(avps[pos]) << 24) | (uint32_t(avps[pos + 1]) << 16) |
                         (uint32_t(avps[pos + 2]) << 8) | avps[pos + 3]);
        const std::size_t len = (std::size_t(avps[pos + 5]) << 16) | (std::size_t(avps[pos + 6]) << 8) | avps[pos + 7];
        pos += (len + 3) & ~std::size_t(3);
    }
    EXPECT_EQ(pos, avps.size());  // whole AVPs, padded
    return result;
}

std::string encodeError(const nlohmann::json& extra) {
    std::vector<uint8_t> out;
    std::vector<std::string> dropped;
    std::string err;
    EXPECT_FALSE(encodeCapabilitiesAvps(extra, base(), Built, out, dropped, err));
    return err;
}
}  // namespace

TEST(CapabilitiesAvps, NoneByDefault) {
    std::vector<uint8_t> out{1};
    std::vector<std::string> dropped;
    std::string err;
    EXPECT_TRUE(encodeCapabilitiesAvps(nullptr, base(), Built, out, dropped, err));
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(encodeCapabilitiesAvps(nlohmann::json::object(), base(), Built, out, dropped, err));
    EXPECT_TRUE(out.empty());
}

TEST(CapabilitiesAvps, EncodesPlainRepeatedAndGroupedAvps) {
    ASSERT_NE(base().getAvp("Supported-Vendor-Id"), nullptr) << "base dictionary without Supported-Vendor-Id";
    const auto extra = R"({
        "Supported-Vendor-Id": [10415, 5535],
        "Firmware-Revision": 7,
        "Vendor-Specific-Application-Id": {"Vendor-Id": 10415, "Auth-Application-Id": 16777238}
    })"_json;
    std::vector<uint8_t> out;
    std::vector<std::string> dropped;
    std::string err;
    ASSERT_TRUE(encodeCapabilitiesAvps(extra, base(), Built, out, dropped, err)) << err;
    auto c = codes(out);
    std::sort(c.begin(), c.end());
    EXPECT_EQ(c, (std::vector<uint32_t>{260, 265, 265, 267}));  // the grouped one keeps its inner AVPs inside
    EXPECT_TRUE(dropped.empty());
}

TEST(CapabilitiesAvps, BuiltAvpsWin) {
    // equal to the built ones: dropped (nothing added)
    std::vector<uint8_t> out;
    std::vector<std::string> dropped;
    std::string err;
    ASSERT_TRUE(encodeCapabilitiesAvps(
        R"({"Origin-Host": "gw.example.com", "Origin-Realm": "example.com", "Product-Name": "h2diagent",
            "Vendor-Id": 0, "Firmware-Revision": 1})"_json,
        base(), Built, out, dropped, err))
        << err;
    EXPECT_EQ(codes(out), (std::vector<uint32_t>{267}));
    std::sort(dropped.begin(), dropped.end());
    EXPECT_EQ(dropped, (std::vector<std::string>{"Origin-Host", "Origin-Realm", "Product-Name", "Vendor-Id"}));

    // different: an error naming the AVP
    EXPECT_NE(encodeError(R"({"Origin-Host": "other.example.com"})"_json).find("'Origin-Host'"), std::string::npos);
    EXPECT_NE(encodeError(R"({"Vendor-Id": 10415})"_json).find("'Vendor-Id'"), std::string::npos);
    // never given
    EXPECT_NE(encodeError(R"({"Auth-Application-Id": 4})"_json).find("'Auth-Application-Id'"), std::string::npos);
    EXPECT_NE(encodeError(R"({"Result-Code": 5010})"_json).find("'Result-Code'"), std::string::npos);
    EXPECT_NE(encodeError(R"({"Host-IP-Address": "10.0.0.1"})"_json).find("'Host-IP-Address'"), std::string::npos);
    EXPECT_NE(encodeError(R"({"_header": {}})"_json).find("_header"), std::string::npos);
}

TEST(CapabilitiesAvps, InvalidContent) {
    EXPECT_FALSE(encodeError(R"([1, 2])"_json).empty());                                 // not an object
    EXPECT_FALSE(encodeError(R"({"No-Such-Avp": 1})"_json).empty());                     // unknown AVP
    EXPECT_FALSE(encodeError(R"({"Firmware-Revision": "not a number"})"_json).empty());  // does not encode
}
