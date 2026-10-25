"""Bounded diagnostic session using the production LAN control protocol.

Requires an already listening, token-authenticated sender. This does not execute
commands or install files on the receiver; its normal GUI handles the request.
"""
import argparse
import json
import socket
import struct
import time
import uuid


def frame(kind, body):
    return struct.pack('!IHHII', 0x4c335053, 1, kind, len(body), 0) + body


def string(value):
    value = value.encode('utf-8')
    return struct.pack('!H', len(value)) + value


def receive(connection):
    def exact(count):
        data = b''
        while len(data) < count:
            part = connection.recv(count - len(data))
            if not part:
                raise RuntimeError('Receiver control connection closed')
            data += part
        return data
    magic, version, kind, length, reserved = struct.unpack('!IHHII', exact(16))
    if magic != 0x4c335053 or version != 1 or length > 512 or reserved:
        raise RuntimeError('Invalid control response')
    return kind, exact(length)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--peer', required=True)
    parser.add_argument('--media-port', type=int, required=True)
    parser.add_argument('--token', required=True)
    parser.add_argument('--seconds', type=int, default=25)
    args = parser.parse_args()
    if len(args.token) != 32 or any(c not in '0123456789abcdef' for c in args.token):
        raise ValueError('Expected 32 lowercase hexadecimal token characters')
    if not 1 <= args.seconds <= 180:
        raise ValueError('Bounded measurement must be 1..180 seconds')
    peer = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as discovery:
        discovery.bind(('0.0.0.0', 48731))
        discovery.settimeout(1)
        until = time.monotonic() + 8
        while time.monotonic() < until:
            try:
                data, address = discovery.recvfrom(1024)
            except socket.timeout:
                continue
            if address[0] != args.peer or len(data) < 58:
                continue
            magic, version, kind, length, reserved = struct.unpack('!IHHII', data[:16])
            if (magic, version, kind, reserved) != (0x4c335053, 1, 1, 0) or length != len(data) - 16:
                continue
            body = data[16:]
            identity = str(uuid.UUID(body[:36].decode('ascii')))
            port, state, name_length = struct.unpack('!HHH', body[36:42])
            if len(body) != 42 + name_length or not port:
                continue
            peer = dict(id=identity, ip=args.peer, controlPort=port, state=state, name=body[42:].decode('utf-8'))
            break
    if not peer or peer['state'] != 0:
        raise RuntimeError('Requested receiver is offline or busy')
    local = str(uuid.uuid4()).encode('ascii')
    token = args.token.encode('ascii')
    accepted = False
    with socket.socket() as own_control, socket.create_connection((args.peer, peer['controlPort']), timeout=12) as connection:
        own_control.bind(('0.0.0.0', 0))
        own_control.listen(1)
        body = local + peer['id'].encode('ascii') + token
        body += struct.pack('!HHI', args.media_port, own_control.getsockname()[1], 10000)
        body += string('PUREDARK-GAMING')
        connection.sendall(frame(2, body))
        kind, response = receive(connection)
        if kind != 3 or len(response) < 72 or response[:36].decode('ascii') != peer['id'] or response[36:68] != token:
            raise RuntimeError('Receiver response identity mismatch')
        decision, size = struct.unpack('!HH', response[68:72])
        if len(response) != 72 + size or decision != 1:
            raise RuntimeError('Receiver rejected: ' + response[72:].decode('utf-8', errors='replace'))
        accepted = True
        print(json.dumps(dict(event='connected', peer=peer, seconds=args.seconds)), flush=True)
        start = time.monotonic()
        beats = 0
        try:
            connection.settimeout(2.5)
            while time.monotonic() - start < args.seconds:
                connection.sendall(frame(4, local + token))
                kind, body = receive(connection)
                if kind != 4 or body != peer['id'].encode('ascii') + token:
                    raise RuntimeError('Receiver ended or changed the session')
                beats += 1
                time.sleep(max(0, min(1, start + args.seconds - time.monotonic())))
            print(json.dumps(dict(event='completed', heartbeatReplies=beats, duration=time.monotonic() - start)), flush=True)
        finally:
            if accepted:
                try:
                    connection.sendall(frame(5, local + token + string('Diagnostic session completed')))
                except OSError:
                    pass


if __name__ == '__main__':
    main()
