/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file CapabilitiesAvps.hpp
 * @brief Extra CER / CEA AVPs (--cer / --cea): json -> encoded AVPs.
 *
 * The gateway (diametercomm) builds the capabilities exchange from its options:
 * Origin-Host, Origin-Realm, Host-IP-Address, Vendor-Id, Product-Name, one
 * Auth-Application-Id per loaded dictionary and, in the CEA, Result-Code. A
 * --cer / --cea json file only ADDS AVPs to them (Supported-Vendor-Id,
 * Firmware-Revision, Inband-Security-Id, Vendor-Specific-Application-Id...),
 * written in the gateway AVP <-> json mapping. The built ones win: a file AVP
 * equal to one of them is dropped, a different one is an error (named), never a
 * silent override; Host-IP-Address, Auth-Application-Id and Result-Code cannot
 * be given at all.
 */

#pragma once

#include <ert/diametercodec/codec/Message.hpp>
#include <ert/diametercodec/stack/Dictionary.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ert {
namespace h2diagent {
namespace helpers {

/** Values the gateway builds into the CER / CEA (the protected AVPs). */
struct CapabilitiesBuilt {
    std::string originHost;
    std::string originRealm;
    std::string productName;
    uint32_t vendorId{0};
};

/**
 * Encodes the extra AVPs of a --cer / --cea json object.
 *
 * @param extra json object: AVP name -> value (gateway mapping); null or {} = none
 * @param dict dictionary holding the base protocol AVPs (any loaded stack)
 * @param built values built by the gateway (protected AVPs)
 * @param out encoded AVPs (each one padded), to append to the CER / CEA
 * @param dropped names of the file AVPs equal to a built one (not added)
 * @param err error (with the AVP named)
 * @return false on a conflict, an invalid object or an AVP which does not encode
 */
inline bool encodeCapabilitiesAvps(const nlohmann::json& extra, const diametercodec::stack::Dictionary& dict,
                                   const CapabilitiesBuilt& built, std::vector<uint8_t>& out,
                                   std::vector<std::string>& dropped, std::string& err) {
    out.clear();
    dropped.clear();
    if (extra.is_null() || (extra.is_object() && extra.empty())) return true;
    if (!extra.is_object()) {
        err = "a json object (AVP name -> value) is expected";
        return false;
    }

    nlohmann::json avps = nlohmann::json::object();
    for (const auto& [name, value] : extra.items()) {
        if (name == "_header") {
            err = "'_header' is not allowed (the gateway builds the message header)";
            return false;
        }
        if (name == "Host-IP-Address" || name == "Auth-Application-Id" || name == "Result-Code") {
            err = "AVP '" + name + "' is built by the gateway (" +
                  (name == "Auth-Application-Id" ? std::string("one per loaded dictionary")
                   : name == "Result-Code"       ? std::string("the CEA outcome")
                                                 : std::string("its local address")) +
                  ") and cannot be given";
            return false;
        }
        nlohmann::json builtValue;
        if (name == "Origin-Host") builtValue = built.originHost;
        if (name == "Origin-Realm") builtValue = built.originRealm;
        if (name == "Product-Name") builtValue = built.productName;
        if (name == "Vendor-Id") builtValue = built.vendorId;
        if (!builtValue.is_null()) {
            if (value != builtValue) {
                err = "AVP '" + name + "' (" + value.dump() + ") differs from the one built by the gateway (" +
                      builtValue.dump() + "): change the command line option instead";
                return false;
            }
            dropped.push_back(name);
            continue;
        }
        avps[name] = value;
    }
    if (avps.empty()) return true;

    nlohmann::json message = avps;
    message["_header"] = {{"version", 1}, {"command-code", 257}, {"request", true}, {"application-id", 0}};
    try {
        const diametercodec::core::Buffer encoded = diametercodec::codec::Message::fromJson(message, dict).encode(dict);
        if (encoded.size() < 20) {
            err = "encoding failed";
            return false;
        }
        out.assign(encoded.begin() + 20, encoded.end());  // the AVPs, without the message header
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

}  // namespace helpers
}  // namespace h2diagent
}  // namespace ert
