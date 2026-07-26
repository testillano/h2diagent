#!/usr/bin/env python3
"""
xml2json-stack.py - Convert anna-style XML Diameter dictionary to diametercodec JSON format.

Usage: python3 xml2json-stack.py <input.xml> [output.json]
"""
import sys
import json
import xml.etree.ElementTree as ET
import re

def parse_dictionary(xml_file):
    tree = ET.parse(xml_file)
    root = tree.getroot()

    # Extract name and application-id
    dict_name = root.get("name", "")
    app_id = 0
    # Try to extract app-id from name pattern "... Application-Id: NNNNN"
    m = re.search(r'Application-Id:\s*(\d+)', dict_name)
    if m:
        app_id = int(m.group(1))
    # Clean name
    dict_name = re.sub(r'\s*\|.*', '', dict_name).strip()

    # Vendors
    vendors = []
    for v in root.findall("vendor"):
        vendors.append({
            "name": v.get("name"),
            "code": int(v.get("code"))
        })

    # Build vendor code->name map for AVP lookup
    vendor_code_to_name = {v["code"]: v["name"] for v in vendors}

    # AVPs
    avps = []
    for avp_elem in root.findall("avp"):
        avp = {}
        avp["name"] = avp_elem.get("name")
        avp["code"] = int(avp_elem.get("code"))

        # Vendor
        vendor_name_attr = avp_elem.get("vendor-name")
        if vendor_name_attr:
            avp["vendor-name"] = vendor_name_attr

        # Flags
        v_bit = avp_elem.get("v-bit", "mustnot")
        m_bit = avp_elem.get("m-bit", "mustnot")

        if v_bit == "must":
            avp["v-bit"] = True
            # If v-bit is set but no vendor-name, try to infer from context
            if "vendor-name" not in avp:
                # Default to 3GPP for vendor-specific AVPs without explicit vendor
                if any(v["name"] == "3GPP" for v in vendors):
                    avp["vendor-name"] = "3GPP"

        if m_bit == "must":
            avp["m-bit"] = True

        # Content: single or grouped
        single_elem = avp_elem.find("single")
        grouped_elem = avp_elem.find("grouped")

        if single_elem is not None:
            single = {"format": single_elem.get("format-name")}

            # Enum
            enum_attr = single_elem.get("enum")
            if enum_attr:
                single["enum"] = enum_attr

            # Labels
            labels = []
            for label in single_elem.findall("label"):
                labels.append({
                    "data": label.get("data"),
                    "alias": label.get("alias")
                })
            if labels:
                single["label"] = labels

            avp["single"] = single

        elif grouped_elem is not None:
            avprules = []
            for rule in grouped_elem.findall("avprule"):
                r = {
                    "type": rule.get("type"),
                    "name": rule.get("id")
                }
                qual = rule.get("qual")
                if qual:
                    r["qual"] = qual
                avprules.append(r)
            avp["grouped"] = {"avprule": avprules}

        avps.append(avp)

    # Commands
    commands = []
    for cmd_elem in root.findall("command"):
        cmd = {}
        cmd["name"] = cmd_elem.get("name")
        cmd["code"] = int(cmd_elem.get("code"))

        # Flags
        if cmd_elem.get("r-bit") == "yes":
            cmd["r-bit"] = True
        if cmd_elem.get("p-bit") == "yes":
            cmd["p-bit"] = True

        # Application-Id
        cmd_app_id = cmd_elem.get("application-id")
        if cmd_app_id:
            cmd["application-id"] = int(cmd_app_id)

        # Avprules
        avprules = []
        for rule in cmd_elem.findall("avprule"):
            r = {
                "type": rule.get("type"),
                "name": rule.get("id")
            }
            qual = rule.get("qual")
            if qual:
                r["qual"] = qual
            avprules.append(r)
        if avprules:
            cmd["avprule"] = avprules

        commands.append(cmd)

    # Build result
    result = {"name": dict_name}
    if app_id:
        result["application-id"] = app_id
    if vendors:
        result["vendor"] = vendors
    if avps:
        result["avp"] = avps
    if commands:
        result["command"] = commands

    return result

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input.xml> [output.json]", file=sys.stderr)
        sys.exit(1)

    result = parse_dictionary(sys.argv[1])
    output = json.dumps(result, indent=2, ensure_ascii=False)

    if len(sys.argv) >= 3:
        with open(sys.argv[2], 'w') as f:
            f.write(output + '\n')
        print(f"Converted: {sys.argv[1]} -> {sys.argv[2]} ({len(result.get('avp',[]))} AVPs, {len(result.get('command',[]))} commands)")
    else:
        print(output)
