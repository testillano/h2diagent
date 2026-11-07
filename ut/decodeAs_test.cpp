/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <gtest/gtest.h>

#include <ert/diametercodec/json/stacks.hpp>
#include <ert/h2diagent/DecodeAs.hpp>

using namespace ert::h2diagent::helpers;
using ert::diametercodec::codec::DecodeOptions;
using ert::diametercodec::stack::Dictionary;

TEST(DecodeAs, ParseAcceptsEveryForm) {
    for (const char* form : {"text", "ipv4", "ipv6", "ipv6Prefix", "msTimeZone", "uli"}) {
        DecodeAsEntry entry;
        std::string err;
        ASSERT_TRUE(parseDecodeAs(std::string("Called-Station-Id=") + form, entry, err)) << err;
        EXPECT_EQ(entry.first, "Called-Station-Id");
        EXPECT_EQ(entry.second, form);
    }
}

TEST(DecodeAs, ParseRejectsMalformed) {
    for (const char* arg : {"Called-Station-Id", "=text", "Called-Station-Id=", "Called-Station-Id=hex",
                            "Called-Station-Id=Text", "Called-Station-Id=base64"}) {
        DecodeAsEntry entry;
        std::string err;
        EXPECT_FALSE(parseDecodeAs(arg, entry, err)) << arg;
        EXPECT_FALSE(err.empty()) << arg;
    }
}

class DecodeAsResolve : public ::testing::Test {
   protected:
    std::map<uint32_t, Dictionary> dictionaries_;
    DecodeAsResolve() { dictionaries_[0].load(ert::diametercodec::json::stacks::base); }
};

TEST_F(DecodeAsResolve, NamesBecomeAvpIds) {
    DecodeOptions options;
    std::string err;
    ASSERT_TRUE(resolveDecodeAs({{"NAS-IP-Address", "ipv4"}, {"Class", "text"}}, dictionaries_, options, err)) << err;
    EXPECT_EQ(options.forms.size(), 2u);
    EXPECT_EQ(options.forms.at({4, 0}), "ipv4");
    EXPECT_EQ(options.forms.at({25, 0}), "text");
}

TEST_F(DecodeAsResolve, NoEntriesIsEmpty) {
    DecodeOptions options;
    std::string err;
    ASSERT_TRUE(resolveDecodeAs({}, dictionaries_, options, err));
    EXPECT_TRUE(options.empty());
}

TEST_F(DecodeAsResolve, NameInSeveralDictionaries) {
    dictionaries_[4].load(ert::diametercodec::json::stacks::base);  // a second stack holding the same AVP
    DecodeOptions options;
    std::string err;
    ASSERT_TRUE(resolveDecodeAs({{"Class", "text"}}, dictionaries_, options, err)) << err;
    EXPECT_EQ(options.forms.size(), 1u);  // same id in both: one entry
    EXPECT_EQ(options.forms.at({25, 0}), "text");
}

TEST_F(DecodeAsResolve, Rejects) {
    const std::vector<std::pair<std::vector<DecodeAsEntry>, std::string>> table = {
        {{{"No-Such-AVP", "text"}}, "not found"},
        {{{"Origin-Host", "text"}}, "not an OctetString"},      // DiameterIdentity
        {{{"Host-IP-Address", "ipv4"}}, "not an OctetString"},  // Address: already readable
        {{{"Class", "text"}, {"Class", "ipv4"}}, "two forms"},
    };
    for (const auto& row : table) {
        DecodeOptions options;
        std::string err;
        EXPECT_FALSE(resolveDecodeAs(row.first, dictionaries_, options, err)) << row.second;
        EXPECT_NE(err.find(row.second), std::string::npos) << err;
    }
}

TEST_F(DecodeAsResolve, SameEntryTwiceIsAccepted) {
    DecodeOptions options;
    std::string err;
    EXPECT_TRUE(resolveDecodeAs({{"Class", "text"}, {"Class", "text"}}, dictionaries_, options, err)) << err;
}
