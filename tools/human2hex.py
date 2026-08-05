#!/usr/bin/env python3
# =============================================================================
# human2hex.py - Convert human-readable values to the hex string expected by
# h2diagent/h2agent for Diameter OctetString AVPs.
#
# In the JSON exchanged with h2agent, OctetString AVPs are written as a plain
# hex string (e.g. Framed-IP-Address 192.168.0.1 -> "c0a80001"). This helper
# builds that hex for the most common "human" Diameter data types.
#
# Usage:
#   human2hex.py --type ipv4  192.168.0.1
#   human2hex.py --type ipv6  2001:db8::1
#   human2hex.py --type ascii "sgsn.example.com"
#   human2hex.py --type uint  --bytes 4 5004
#   human2hex.py --type int   --bytes 4 -1
#   human2hex.py --type time  1700000000        # UNIX epoch -> NTP 4-byte hex
#   human2hex.py --type address 192.168.0.1     # Diameter Address: family+addr
#
# Add --prefix to get a leading "0x".
# =============================================================================
import argparse
import socket
import sys

NTP_EPOCH_OFFSET = 2208988800  # seconds between 1900-01-01 and 1970-01-01


def ipv4_hex(value: str) -> bytes:
    return socket.inet_pton(socket.AF_INET, value)


def ipv6_hex(value: str) -> bytes:
    return socket.inet_pton(socket.AF_INET6, value)


def ascii_hex(value: str) -> bytes:
    return value.encode("utf-8")


def uint_hex(value: str, nbytes: int) -> bytes:
    v = int(value, 0)
    if v < 0:
        raise ValueError("uint must be non-negative")
    return v.to_bytes(nbytes, "big")  # raises OverflowError if it does not fit


def int_hex(value: str, nbytes: int) -> bytes:
    v = int(value, 0)
    return v.to_bytes(nbytes, "big", signed=True)


def time_hex(value: str) -> bytes:
    # Diameter Time = 4-byte NTP timestamp (seconds since 1900).
    epoch = int(value, 0)
    return (epoch + NTP_EPOCH_OFFSET).to_bytes(4, "big")


def address_hex(value: str) -> bytes:
    # Diameter Address = 2-byte address family + address bytes.
    # (Only needed if the AVP is declared Address AND you want to hand-encode
    #  it; normally Address AVPs take the human string directly.)
    try:
        return b"\x00\x01" + socket.inet_pton(socket.AF_INET, value)
    except OSError:
        return b"\x00\x02" + socket.inet_pton(socket.AF_INET6, value)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Convert human-readable values to hex for Diameter OctetString AVPs.")
    p.add_argument("--type", required=True,
                   choices=["ipv4", "ipv6", "ascii", "utf8", "uint", "int", "time", "address"],
                   help="Source data type of the human value.")
    p.add_argument("--bytes", type=int, default=4, dest="nbytes",
                   help="Width in bytes for uint/int (default: 4).")
    p.add_argument("--prefix", action="store_true", help="Prepend '0x' to the output.")
    p.add_argument("value", help="Human-readable value (quote it if it has spaces).")
    args = p.parse_args(argv)

    try:
        if args.type == "ipv4":
            raw = ipv4_hex(args.value)
        elif args.type == "ipv6":
            raw = ipv6_hex(args.value)
        elif args.type in ("ascii", "utf8"):
            raw = ascii_hex(args.value)
        elif args.type == "uint":
            raw = uint_hex(args.value, args.nbytes)
        elif args.type == "int":
            raw = int_hex(args.value, args.nbytes)
        elif args.type == "time":
            raw = time_hex(args.value)
        elif args.type == "address":
            raw = address_hex(args.value)
        else:  # pragma: no cover
            raise ValueError("unsupported type")
    except (OSError, ValueError, OverflowError) as e:
        print(f"error: cannot convert '{args.value}' as {args.type}: {e}", file=sys.stderr)
        return 1

    print(("0x" if args.prefix else "") + raw.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
