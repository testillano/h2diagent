/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file DecodeAs.hpp
 * @brief --decode-as: readable decoding of OctetString AVPs towards h2agent.
 *
 * By default an OctetString AVP reaches h2agent as the hex of its bytes (the
 * dictionary does not say what the bytes hold). '--decode-as <AVP>=<form>'
 * names, per AVP, the readable form of its bytes (text|ipv4|ipv6|ipv6Prefix|
 * msTimeZone|uli): such an AVP is decoded as {"<form>": ...}, or {"hex": ...}
 * when its bytes do not fit (see diametercodec::codec::DecodeOptions).
 *
 * Names are resolved once, at startup, in every loaded dictionary: an AVP
 * unknown in all of them, or not declared OctetString, is a startup error.
 */

#pragma once

#include <ert/diametercodec/codec/Avp.hpp>
#include <ert/diametercodec/stack/Avp.hpp>
#include <ert/diametercodec/stack/Dictionary.hpp>
#include <ert/diametercodec/stack/Format.hpp>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ert {
namespace h2diagent {
namespace helpers {

/** One '--decode-as' entry: AVP name and form. */
using DecodeAsEntry = std::pair<std::string, std::string>;

/**
 * Parse '<AVP>=<form>'. False (and 'err') when there is no '=', an empty side,
 * or an unknown form.
 */
inline bool parseDecodeAs(const std::string& arg, DecodeAsEntry& entry, std::string& err) {
    const auto eq = arg.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == arg.size()) {
        err = "expected <AVP>=<form>, got '" + arg + "'";
        return false;
    }
    entry = {arg.substr(0, eq), arg.substr(eq + 1)};
    if (!diametercodec::codec::DecodeOptions::isForm(entry.second)) {
        err = "unknown form '" + entry.second + "' for '" + entry.first +
              "' (expected text|ipv4|ipv6|ipv6Prefix|msTimeZone|uli)";
        return false;
    }
    return true;
}

/**
 * Resolve the entries against the loaded dictionaries into codec options (AVP
 * id -> form). A name present in several dictionaries is configured in all of
 * them. False (and 'err') for an AVP unknown in every dictionary, not declared
 * OctetString, or configured twice with different forms.
 */
inline bool resolveDecodeAs(const std::vector<DecodeAsEntry>& entries,
                            const std::map<uint32_t, diametercodec::stack::Dictionary>& dictionaries,
                            diametercodec::codec::DecodeOptions& options, std::string& err) {
    options.forms.clear();
    for (const auto& [name, form] : entries) {
        bool found = false;
        for (const auto& [appId, dict] : dictionaries) {
            const diametercodec::stack::Avp* avp = dict.getAvp(name);
            if (!avp) continue;
            found = true;
            const diametercodec::stack::Format* fmt = avp->getFormat();
            if (!fmt || !fmt->isOctetString()) {
                err = "AVP '" + name + "' is not an OctetString (application-id " + std::to_string(appId) +
                      "): only OctetString AVPs have readable forms";
                return false;
            }
            auto [it, inserted] = options.forms.emplace(avp->getId(), form);
            if (!inserted && it->second != form) {
                err = "AVP '" + name + "' configured with two forms ('" + it->second + "', '" + form + "')";
                return false;
            }
        }
        if (!found) {
            err = "AVP '" + name + "' not found in the loaded dictionaries";
            return false;
        }
    }
    return true;
}

}  // namespace helpers
}  // namespace h2diagent
}  // namespace ert
