/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <gtest/gtest.h>

#include <ert/h2diagent/CommandNames.hpp>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

using namespace ert::h2diagent::helpers;

// =============================================================================
// commandAbbreviation
// =============================================================================
TEST(CommandNames, AbbreviationUpperCasePrefixTakenWhole) {
    EXPECT_EQ(commandAbbreviation("AA-Request"), "AAR");
    EXPECT_EQ(commandAbbreviation("AA-Answer"), "AAA");
    EXPECT_EQ(commandAbbreviation("CC-Request"), "CCR");
    EXPECT_EQ(commandAbbreviation("RA-Answer"), "RAA");
    EXPECT_EQ(commandAbbreviation("SL-Request"), "SLR");
}

TEST(CommandNames, AbbreviationInitialsOfLongNames) {
    EXPECT_EQ(commandAbbreviation("Re-Auth-Request"), "RAR");
    EXPECT_EQ(commandAbbreviation("Capabilities-Exchange-Answer"), "CEA");
    EXPECT_EQ(commandAbbreviation("Session-Termination-Request"), "STR");
    EXPECT_EQ(commandAbbreviation("Device-Watchdog-Answer"), "DWA");
    EXPECT_EQ(commandAbbreviation("Spending-Status-Notification-Request"), "SSNR");
}

TEST(CommandNames, AbbreviationMixedWords) {
    EXPECT_EQ(commandAbbreviation("AA-Mobile-Node-Request"), "AAMNR");
    EXPECT_EQ(commandAbbreviation("lower-case-answer-Answer"), "LCAA");
}

TEST(CommandNames, AbbreviationAlreadyAbbreviatedKept) {
    EXPECT_EQ(commandAbbreviation("CER"), "CER");
    EXPECT_EQ(commandAbbreviation("DWA"), "DWA");
    EXPECT_EQ(commandAbbreviation("Foo"), "Foo");
}

TEST(CommandNames, AbbreviationDegenerateNamesKept) {
    EXPECT_EQ(commandAbbreviation(""), "");
    EXPECT_EQ(commandAbbreviation("-Request"), "-Request");      // no prefix
    EXPECT_EQ(commandAbbreviation("-Answer"), "-Answer");        // no prefix
    EXPECT_EQ(commandAbbreviation("--Request"), "--Request");    // only dashes
    EXPECT_EQ(commandAbbreviation("CC-request"), "CC-request");  // suffix is case sensitive
}

TEST(CommandNames, AbbreviationEmptyWordsSkipped) {
    EXPECT_EQ(commandAbbreviation("Re--Auth-Request"), "RAR");
    EXPECT_EQ(commandAbbreviation("-Re-Auth-Request"), "RAR");
}

// =============================================================================
// collectCommandNames
// =============================================================================
namespace {
nlohmann::json command(const std::string& name, uint32_t code, bool request) {
    nlohmann::json c = {{"name", name}, {"code", code}};
    if (request) c["r-bit"] = true;
    return c;
}
}  // namespace

TEST(CommandNames, CollectRequestAndAnswer) {
    const nlohmann::json dict = {{"command",
                                  {command("AA-Request", 265, true), command("AA-Answer", 265, false),
                                   command("CER", 257, true), command("CEA", 257, false)}}};
    std::map<uint32_t, CommandNames> names;
    EXPECT_TRUE(collectCommandNames(dict, names).empty());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[265].request, "AAR");
    EXPECT_EQ(names[265].answer, "AAA");
    EXPECT_EQ(names[257].request, "CER");
    EXPECT_EQ(names[257].answer, "CEA");
}

TEST(CommandNames, CollectRbitFalseOrMissingIsAnswer) {
    nlohmann::json explicitFalse = command("ST-Answer", 275, false);
    explicitFalse["r-bit"] = false;
    nlohmann::json notBoolean = command("AS-Answer", 274, false);
    notBoolean["r-bit"] = "yes";
    const nlohmann::json dict = {{"command", {explicitFalse, notBoolean}}};
    std::map<uint32_t, CommandNames> names;
    collectCommandNames(dict, names);
    EXPECT_EQ(names[275].answer, "STA");
    EXPECT_TRUE(names[275].request.empty());
    EXPECT_EQ(names[274].answer, "ASA");
    EXPECT_TRUE(names[274].request.empty());
}

TEST(CommandNames, CollectOnlyOneSide) {
    const nlohmann::json dict = {{"command", {command("RA-Request", 258, true)}}};
    std::map<uint32_t, CommandNames> names;
    collectCommandNames(dict, names);
    EXPECT_EQ(names[258].request, "RAR");
    EXPECT_TRUE(names[258].answer.empty());
}

TEST(CommandNames, CollectFirstDictionaryWinsAndReportsConflicts) {
    std::map<uint32_t, CommandNames> names;
    const nlohmann::json first = {{"command", {command("RA-Request", 258, true), command("RA-Answer", 258, false)}}};
    const nlohmann::json same = {{"command", {command("RAR", 258, true), command("Re-Auth-Answer", 258, false)}}};
    const nlohmann::json other = {{"command", {command("XY-Request", 258, true), command("RA-Answer", 258, false)}}};

    EXPECT_TRUE(collectCommandNames(first, names).empty());
    EXPECT_TRUE(collectCommandNames(same, names).empty());  // same abbreviation from another name: no conflict
    const auto conflicts = collectCommandNames(other, names);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0], "258 request: RAR kept, XYR ignored");
    EXPECT_EQ(names[258].request, "RAR");
    EXPECT_EQ(names[258].answer, "RAA");
}

TEST(CommandNames, CollectSkipsInvalidEntries) {
    const nlohmann::json dict = {{"command",
                                  {nlohmann::json::array(),                        // not an object
                                   {{"code", 265}, {"r-bit", true}},               // no name
                                   {{"name", 265}, {"code", 265}},                 // name not a string
                                   {{"name", "AA-Request"}, {"r-bit", true}},      // no code
                                   {{"name", "AA-Request"}, {"code", "265"}},      // code not a number
                                   {{"name", "AA-Request"}, {"code", -265}},       // negative code
                                   {{"name", "AA-Request"}, {"code", 265.5}},      // float code
                                   {{"name", "AA-Request"}, {"code", 0x1000000}},  // over 24 bits
                                   {{"name", ""}, {"code", 266}},                  // empty name
                                   command("CC-Request", 0xFFFFFF, true)}}};       // 24-bit limit is valid
    std::map<uint32_t, CommandNames> names;
    EXPECT_TRUE(collectCommandNames(dict, names).empty());
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0xFFFFFF].request, "CCR");
}

TEST(CommandNames, CollectWithoutCommands) {
    std::map<uint32_t, CommandNames> names;
    EXPECT_TRUE(collectCommandNames(nlohmann::json::object(), names).empty());
    EXPECT_TRUE(collectCommandNames({{"command", "none"}}, names).empty());
    EXPECT_TRUE(collectCommandNames(nlohmann::json::array(), names).empty());
    EXPECT_TRUE(collectCommandNames(nlohmann::json(), names).empty());
    EXPECT_TRUE(names.empty());
}

// The stacks shipped in tools/stacks (what the chart bundles) give the usual
// abbreviations without conflicts. Read from the source tree: skipped where it
// is not reachable (e.g. the unit-test image).
TEST(CommandNames, ShippedStacksGiveTheUsualAbbreviations) {
    std::map<uint32_t, CommandNames> names;
    bool loaded = false;
    for (const char* stack : {"gx", "rx", "sy"}) {
        for (const std::string dir : {"tools/stacks/", "../tools/stacks/", "../../tools/stacks/"}) {
            std::ifstream ifs(dir + stack + ".json");
            if (!ifs.is_open()) continue;
            EXPECT_TRUE(collectCommandNames(nlohmann::json::parse(ifs), names).empty()) << stack;
            loaded = true;
            break;
        }
    }
    if (!loaded) GTEST_SKIP() << "tools/stacks not reachable from the working directory";

    const std::map<uint32_t, std::pair<std::string, std::string>> expected = {
        {257, {"CER", "CEA"}}, {258, {"RAR", "RAA"}},     {265, {"AAR", "AAA"}},    {271, {"ACR", "ACA"}},
        {272, {"CCR", "CCA"}}, {274, {"ASR", "ASA"}},     {275, {"STR", "STA"}},    {280, {"DWR", "DWA"}},
        {282, {"DPR", "DPA"}}, {8388635, {"SLR", "SLA"}}, {8388636, {"SNR", "SNA"}}};
    for (const auto& [code, pair] : expected) {
        EXPECT_EQ(names[code].request, pair.first) << code;
        EXPECT_EQ(names[code].answer, pair.second) << code;
    }
}
