# SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
# SPDX-License-Identifier: MIT

"""Verify two synthetic TLS peers through a standalone hub, without desktop input."""

import argparse
import pathlib
import socket
import ssl
import struct
import subprocess
import tempfile
import time


HEADER = struct.Struct(">4sHHHHIQ")
LEASE = struct.Struct(">BIH")
EVENT = struct.Struct(">Bhh")
HELLO = 1
POLL = 11
STATE = 12
WINDOWS_RELAY = b"\x02\x02\x01"


def read_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError("hub disconnected")
        data.extend(chunk)
    return data


class Peer:
    def __init__(self, connection):
        self.connection = connection
        self.sent = 0
        self.received = 0

    def send(self, kind, payload):
        self.sent += 1
        header = HEADER.pack(b"DFEX", 1, 0, kind, 0, len(payload), self.sent)
        self.connection.sendall(header + payload)

    def receive(self, kind):
        self.received += 1
        magic, major, minor, actual, flags, size, sequence = HEADER.unpack(
            read_exact(self.connection, HEADER.size)
        )
        assert (magic, major, minor, actual, flags, sequence) == (
            b"DFEX", 1, 0, kind, 0, self.received
        )
        assert size <= 327
        return read_exact(self.connection, size)


def connect(port, identity, hub_identity):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = False
    context.load_verify_locations(cafile=str(hub_identity))
    context.load_cert_chain(certfile=str(identity))
    deadline = time.monotonic() + 4
    while True:
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=2)
            return context.wrap_socket(connection, server_hostname="localhost")
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def verify_relay(left, right):
    right.send(HELLO, WINDOWS_RELAY)
    assert left.receive(HELLO) == right.receive(HELLO) == WINDOWS_RELAY

    def round_trip(first, second):
        left.send(POLL, first)
        right.send(POLL, second)
        return left.receive(STATE), right.receive(STATE)

    request = LEASE.pack(1, 0, 320)
    idle = LEASE.pack(0, 0, 0)
    assert round_trip(request, idle) == (
        LEASE.pack(2, 1, 320), LEASE.pack(1, 1, 320)
    )
    idle = LEASE.pack(0, 1, 0)
    assert round_trip(idle, LEASE.pack(4, 1, 0)) == (
        LEASE.pack(0x82, 1, 320), LEASE.pack(0x81, 1, 320)
    )
    motion = EVENT.pack(1, 11, 19)
    assert round_trip(idle + motion, idle) == (
        LEASE.pack(0x82, 1, 320), LEASE.pack(0x81, 1, 320) + motion
    )
    assert round_trip(idle, LEASE.pack(2, 1, 0)) == (
        LEASE.pack(0, 1, 320), LEASE.pack(0, 1, 320)
    )
    assert round_trip(idle, LEASE.pack(1, 1, 640)) == (
        LEASE.pack(1, 2, 640), LEASE.pack(2, 2, 640)
    )
    idle = LEASE.pack(0, 2, 0)
    assert round_trip(LEASE.pack(4, 2, 0), idle) == (
        LEASE.pack(0x81, 2, 640), LEASE.pack(0x82, 2, 640)
    )
    assert round_trip(idle, idle + motion) == (
        LEASE.pack(0x81, 2, 640) + motion, LEASE.pack(0x82, 2, 640)
    )
    right.connection.close()
    assert left.connection.recv(1) == b"", "disconnected target left the other peer attached"


def run(core):
    with tempfile.TemporaryDirectory(prefix="deskflow-hub-") as directory:
        files = [pathlib.Path(directory) / f"{name}.pem" for name in ("hub", "a", "b")]
        pins = [
            subprocess.check_output(
                [core, "--enhanced-files", "identity", "--certificate", str(path)],
                text=True, timeout=8,
            ).strip()
            for path in files
        ]
        with socket.socket() as first, socket.socket() as second:
            first.bind(("127.0.0.1", 0))
            second.bind(("127.0.0.1", 0))
            port_a = first.getsockname()[1]
            port_b = second.getsockname()[1]

        hub = subprocess.Popen(
            [core, "--enhanced-hub", "--address", "127.0.0.1", "--certificate", str(files[0]),
             "--peer-a", pins[1], "--peer-b", pins[2], "--port-a", str(port_a),
             "--port-b", str(port_b), "--family", "windows"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            with connect(port_a, files[1], files[0]) as a:
                # A must send HELLO before the hub begins authenticating B.
                left = Peer(a)
                left.send(HELLO, WINDOWS_RELAY)
                with connect(port_b, files[2], files[0]) as b:
                    verify_relay(left, Peer(b))
            hub.wait(timeout=4)
        finally:
            if hub.poll() is None:
                hub.terminate()
                hub.wait(timeout=4)
            print(hub.stdout.read(), end="")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=pathlib.Path, help="path to the newly built deskflow-core executable")
    run(str(parser.parse_args().core.resolve(strict=True)))
    print("Hub relayed both directions and disconnected the remaining peer.")
