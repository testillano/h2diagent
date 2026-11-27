/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file CommandNames.hpp
 * @brief Readable command names (AAR, AAA, ...) of the loaded dictionaries.
 *
 * The Diameter metrics only carry the numeric 'command_code'. The gateway
 * publishes, per command of its dictionaries, an info gauge with the request
 * and answer abbreviations, so a dashboard can join it and show "AAR" next to
 * 265 (see h2diagent_diameter_command_info in the README).
 *
 * The abbreviation comes from the dictionary name: "<prefix>-Request" and
 * "<prefix>-Answer" give the prefix initials plus R / A, an upper case word
 * of the prefix being taken whole ("AA-Request" -> AAR, "Re-Auth-Answer" ->
 * RAA). Any other name is already the abbreviation ("CER") and is kept.
 */

#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ert {
namespace h2diagent {
namespace helpers {

/** Request and answer abbreviations of a command code (empty: not in the dictionaries). */
struct CommandNames {
    std::string request;
    std::string answer;
};

/** Command codes are 24 bits in the Diameter header. */
constexpr uint64_t MaxCommandCode = 0xFFFFFF;

namespace detail {

inline bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A word without lower case letters ("AA", "SL") is already abbreviated.
inline bool isUpperWord(const std::string& word) {
    for (unsigned char c : word)
        if (std::islower(c)) return false;
    return true;
}

}  // namespace detail

/**
 * Abbreviation of a dictionary command name: "CC-Request" -> "CCR",
 * "Re-Auth-Answer" -> "RAA", "Capabilities-Exchange-Request" -> "CER". A name
 * without the -Request / -Answer suffix is returned as is ("CER", "DWA").
 */
inline std::string commandAbbreviation(const std::string& name) {
    static const std::string RequestSuffix = "-Request";
    static const std::string AnswerSuffix = "-Answer";

    std::string prefix;
    char kind = 0;
    if (detail::endsWith(name, RequestSuffix)) {
        prefix = name.substr(0, name.size() - RequestSuffix.size());
        kind = 'R';
    } else if (detail::endsWith(name, AnswerSuffix)) {
        prefix = name.substr(0, name.size() - AnswerSuffix.size());
        kind = 'A';
    } else {
        return name;
    }

    std::string abbreviation;
    std::size_t start = 0;
    while (start <= prefix.size()) {
        std::size_t end = prefix.find('-', start);
        if (end == std::string::npos) end = prefix.size();
        const std::string word = prefix.substr(start, end - start);
        if (!word.empty()) {
            if (detail::isUpperWord(word))
                abbreviation += word;
            else
                abbreviation += static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        }
        start = end + 1;
    }
    if (abbreviation.empty()) return name;  // only dashes before the suffix
    return abbreviation + kind;
}

/**
 * Add the commands of a dictionary JSON document ("command" array: name,
 * code, r-bit) to 'names'. A side already known keeps its value (the first
 * dictionary loaded wins); every different abbreviation ignored for it is
 * returned as a readable conflict. Entries without a string name or a
 * 24-bit unsigned code are skipped.
 */
inline std::vector<std::string> collectCommandNames(const nlohmann::json& dictionary,
                                                    std::map<uint32_t, CommandNames>& names) {
    std::vector<std::string> conflicts;
    if (!dictionary.is_object()) return conflicts;
    const auto commands = dictionary.find("command");
    if (commands == dictionary.end() || !commands->is_array()) return conflicts;

    for (const auto& command : *commands) {
        if (!command.is_object()) continue;
        const auto name = command.find("name");
        const auto code = command.find("code");
        if (name == command.end() || !name->is_string()) continue;
        if (code == command.end() || !code->is_number_integer()) continue;
        const int64_t codeValue = code->get<int64_t>();  // an unsigned over int64 wraps negative: skipped
        if (codeValue < 0 || static_cast<uint64_t>(codeValue) > MaxCommandCode) continue;
        const auto rbit = command.find("r-bit");
        const bool isRequest = rbit != command.end() && rbit->is_boolean() && rbit->get<bool>();

        const std::string abbreviation = commandAbbreviation(name->get<std::string>());
        if (abbreviation.empty()) continue;
        const uint32_t commandCode = static_cast<uint32_t>(codeValue);
        std::string& side = isRequest ? names[commandCode].request : names[commandCode].answer;
        if (side.empty()) {
            side = abbreviation;
        } else if (side != abbreviation) {
            conflicts.push_back(std::to_string(commandCode) + (isRequest ? " request: " : " answer: ") + side +
                                " kept, " + abbreviation + " ignored");
        }
    }
    return conflicts;
}

}  // namespace helpers
}  // namespace h2diagent
}  // namespace ert
