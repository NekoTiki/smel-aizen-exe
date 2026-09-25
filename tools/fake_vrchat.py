#!/usr/bin/env python3
"""Pretend to be VRChat (or eavesdrop on it). Standard library only.

  Send to the ESP32:
    python tools/fake_vrchat.py 192.168.1.50 set RelayHeadpat true
    python tools/fake_vrchat.py 192.168.1.50 set RelayProximity 0.9
    python tools/fake_vrchat.py 192.168.1.50 tap RelayBoop          # true, wait, false
    python tools/fake_vrchat.py 192.168.1.50 sweep RelayProximity   # float 0 -> 1 -> 0
    python tools/fake_vrchat.py 192.168.1.50 avatar                 # /avatar/change
    python tools/fake_vrchat.py 192.168.1.50 query                  # read OSCQuery HOST_INFO + tree

  Print OSC arriving on a local port:
    python tools/fake_vrchat.py listen          # port 9001: what VRChat sends (close the ESP32 launch
                                                # option first) - handy to find your contact param names
    python tools/fake_vrchat.py listen 9000     # feedback params the ESP32 sends back (VRChat closed)
"""

import argparse
import socket
import struct
import time

PARAM_PREFIX = "/avatar/parameters/"


def _pad(data: bytes) -> bytes:
    return data + b"\0" * (4 - len(data) % 4)  # always at least one NUL


def encode(address: str, value) -> bytes:
    if isinstance(value, bool):
        return _pad(address.encode()) + _pad(b",T" if value else b",F")
    if isinstance(value, int):
        return _pad(address.encode()) + _pad(b",i") + struct.pack(">i", value)
    if isinstance(value, float):
        return _pad(address.encode()) + _pad(b",f") + struct.pack(">f", value)
    return _pad(address.encode()) + _pad(b",s") + _pad(str(value).encode())


def _read_str(data: bytes, pos: int):
    end = data.index(b"\0", pos)
    return data[pos:end].decode(errors="replace"), pos + (end - pos) // 4 * 4 + 4


def decode(data: bytes):
    """Yields (address, [args]) for a message or (nested) bundle."""
    if data.startswith(b"#bundle\0"):
        pos = 16
        while pos + 4 <= len(data):
            (size,) = struct.unpack_from(">i", data, pos)
            yield from decode(data[pos + 4 : pos + 4 + size])
            pos += 4 + size
        return
    address, pos = _read_str(data, 0)
    tags, pos = _read_str(data, pos) if pos < len(data) else (",", pos)
    args = []
    for tag in tags[1:]:
        if tag in "TF":
            args.append(tag == "T")
        elif tag == "i":
            args.append(struct.unpack_from(">i", data, pos)[0])
            pos += 4
        elif tag == "f":
            args.append(round(struct.unpack_from(">f", data, pos)[0], 4))
            pos += 4
        elif tag == "s":
            s, pos = _read_str(data, pos)
            args.append(s)
        else:
            args.append(f"<{tag}>")
            break
    yield address, args


def parse_value(text: str):
    low = text.lower()
    if low in ("true", "on"):
        return True
    if low in ("false", "off"):
        return False
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="ESP32 IP address, or 'listen'")
    ap.add_argument("command", nargs="?", help="set | tap | sweep | avatar | query  (or port number for listen)")
    ap.add_argument("param", nargs="?")
    ap.add_argument("value", nargs="?")
    ap.add_argument("--port", type=int, default=9001, help="ESP32 OSC port (default 9001)")
    ap.add_argument("--oscquery-port", type=int, default=8080, help="ESP32 OSCQuery HTTP port (default 8080)")
    args = ap.parse_args()

    if args.target == "listen":
        port = int(args.command or 9001)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", port))
        print(f"listening for OSC on UDP {port}, Ctrl+C to stop")
        while True:
            data, (host, _) = sock.recvfrom(4096)
            try:
                for address, values in decode(data):
                    print(f"{host:15} {address.removeprefix(PARAM_PREFIX):40} {values}")
            except (ValueError, struct.error) as exc:
                print(f"{host:15} malformed packet ({exc}): {data!r}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.target, args.port)

    def send(address, value):
        sock.sendto(encode(address, value), dest)
        print(f"-> {address} = {value!r}")

    if args.command == "query":
        import json
        import urllib.request

        for path in ("/?HOST_INFO", "/"):
            url = f"http://{args.target}:{args.oscquery_port}{path}"
            with urllib.request.urlopen(url, timeout=3) as resp:
                print(f"{url}\n{json.dumps(json.load(resp), indent=2)}\n")
        return
    if args.command == "avatar":
        send("/avatar/change", "avtr_00000000-0000-0000-0000-000000000000")
        return
    if not args.param:
        ap.error("param is required")
    address = PARAM_PREFIX + args.param

    if args.command == "set":
        if args.value is None:
            ap.error("set needs a value")
        send(address, parse_value(args.value))
    elif args.command == "tap":
        send(address, True)
        time.sleep(0.3)
        send(address, False)
    elif args.command == "sweep":
        steps = [i / 20 for i in range(21)]
        for v in steps + steps[::-1]:
            send(address, float(v))
            time.sleep(0.05)
    else:
        ap.error(f"unknown command {args.command!r}")


if __name__ == "__main__":
    main()
