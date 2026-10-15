#!/usr/bin/env python3
"""r6relay: the internet relay for Rainbow Six co-op. Every player's game dials out to it, so nobody opens a port, a
firewall rule or a router mapping. A host gets a room code; joiners give the code; the relay carries the bytes of each
joiner's TCP connection to the host's game and back. Nothing is parsed past the frame header.

Wire format (relay.c speaks the same; docs/R6-RELAY.md): a frame is [u8 type][u8 peer][u16 length, big-endian] and
the payload.
  01 HELLO  client -> relay   "R6R1 host <build> <CODE|*> <name>" or "R6R1 join <CODE> <build>"; <build>: the first 16
                              hex digits of the game executable's SHA-256 (a join to a host of another build is
                              refused); a host's <CODE> is the room it asks for (the launcher's, shown before the game
                              starts), * for any free one
  02 ROOM   relay -> host     the room's code
  03 OPEN   relay -> host     joiner <peer> arrived;  relay -> joiner (peer 0): you are in
  04 DATA   both ways         host side: peer = the joiner; joiner side: peer 0
  05 CLOSE  both ways         that joiner's connection ended (to a joiner: the host closed it; then the relay hangs up)
  06 PING   07 PONG           either side, any time; a connection silent for IDLE seconds is dropped
  08 ERROR  relay -> client   text ("no such room", "room full", "version", ...); then the relay hangs up
A payload is at most MAX_PAYLOAD bytes (senders split larger reads); an empty DATA is legal. Every socket has TCP_NODELAY.

Behind a proxy that announces each client with the PROXY protocol (v1, the text header: Fly.io's "proxy_proto"
handler), --proxy-protocol takes the client's address from that header; otherwise every player would seem to come from
the proxy and share one address's limits. A connection without a valid header is then dropped.

Usage: r6relay.py [--port 2346 --port 443 ...] [--host 0.0.0.0] [--proxy-protocol]. Standard library only (Python 3.8+).
Deploying it: docs/R6-RELAY.md, "Running the relay" (Dockerfile and fly.toml beside this file)."""
import argparse
import asyncio
import collections
import logging
import secrets
import socket
import struct
import time

HELLO, ROOM, OPEN, DATA, CLOSE, PING, PONG, ERROR = range(1, 9)
VERSION = 'R6R1'
ALPHABET = 'BCDFGHJKMPQRTVWXY2346789'            # no vowels (no words), no 0/O, 1/I/L, 5/S: read out loud safely
CODE_LEN = 6
MAX_PEERS = 15                                   # joiners per room (the game itself allows fewer)
MAX_ROOMS = 2000
PER_IP_CONNS = 32
PER_IP_ROOMS = 4
BAD_JOINS_PER_MIN = 20                           # wrong codes from one address before it is refused for a minute
HELLO_TIMEOUT = 10
IDLE = 60
MAX_PAYLOAD = 4096
WRITE_LIMIT = 256 << 10                          # bytes queued for one connection before it is dropped as stuck

log = logging.getLogger('r6relay')


def canon(code):
    """a room code as typed: case and dashes don't matter"""
    return code.upper().replace('-', '')


def frame(kind, peer=0, payload=b''):
    return struct.pack('>BBH', kind, peer, len(payload)) + payload


class FrameTooLong(Exception): pass


def proxy_source(line):
    """a PROXY protocol v1 header ("PROXY TCP4 <src> <dst> <sport> <dport>\\r\\n"): the client's address; '' for
    "PROXY UNKNOWN" (the proxy's own connection, e.g. a health check); None if it isn't a header"""
    if len(line) > 107 or not line.endswith(b'\r\n'): return None
    p = line[:-2].decode('ascii', 'replace').split(' ')
    if p[:2] == ['PROXY', 'UNKNOWN']: return ''
    if len(p) == 6 and p[0] == 'PROXY' and p[1] in ('TCP4', 'TCP6'): return p[2]
    return None


async def read_frame(reader):
    kind, peer, n = struct.unpack('>BBH', await reader.readexactly(4))
    if n > MAX_PAYLOAD: raise FrameTooLong(n)
    return kind, peer, await reader.readexactly(n) if n else b''


class Conn:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.ip = (writer.get_extra_info('peername') or ('?',))[0]
        self.closed = False
        sock = writer.get_extra_info('socket')
        if sock is not None: sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, kind, peer=0, payload=b''):
        if self.closed: return
        for k in range(0, max(len(payload), 1), MAX_PAYLOAD):   # a payload larger than one frame is split
            self.writer.write(frame(kind, peer, payload[k:k + MAX_PAYLOAD]))
        if self.writer.transport.get_write_buffer_size() > WRITE_LIMIT:
            log.info('%s: not reading, dropped', self.ip); self.close()

    def close(self):
        if not self.closed:
            self.closed = True
            self.writer.close()


class Room:
    def __init__(self, code, host, build):
        self.code, self.host, self.build, self.peers = code, host, build, {}

    def free_peer(self):
        return next((p for p in range(1, MAX_PEERS + 1) if p not in self.peers), 0)


class Relay:
    def __init__(self, proxy_protocol=False):
        self.proxy_protocol = proxy_protocol
        self.rooms = {}
        self.conns = collections.Counter()
        self.hosted = collections.Counter()
        self.bad = collections.defaultdict(collections.deque)

    def new_code(self):
        while True:
            code = ''.join(secrets.choice(ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms: return code

    def refused(self, ip):
        q = self.bad[ip]; now = time.monotonic()
        while q and now - q[0] > 60: q.popleft()
        if not q: del self.bad[ip]
        return len(q) >= BAD_JOINS_PER_MIN

    async def serve(self, reader, writer):
        c = Conn(reader, writer)
        if self.proxy_protocol:                  # the client's own address, before any limit is counted
            async def header():                  # not "PROXY ": dropped at once, not after HELLO_TIMEOUT
                start = await reader.readexactly(6)
                return start + await reader.readuntil(b'\r\n') if start == b'PROXY ' else b''
            try: ip = proxy_source(await asyncio.wait_for(header(), HELLO_TIMEOUT))
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, ConnectionError, OSError): ip = None
            if ip is None: c.close(); return
            if ip: c.ip = ip
        if self.conns[c.ip] >= PER_IP_CONNS: c.close(); return
        self.conns[c.ip] += 1
        try:
            kind, _, body = await asyncio.wait_for(read_frame(reader), HELLO_TIMEOUT)
            words = body.decode('ascii', 'replace').split()
            if kind != HELLO or len(words) < 2 or words[0] != VERSION:
                c.send(ERROR, 0, b'version: this relay speaks ' + VERSION.encode()); return
            if words[1] == 'host' and len(words) >= 5: await self.host(c, words[2].lower(), canon(words[3]))   # name: the rest
            elif words[1] == 'join' and len(words) == 4: await self.join(c, canon(words[2]), words[3].lower())
            else: c.send(ERROR, 0, b'bad hello')
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError, FrameTooLong):
            pass
        finally:
            c.close()
            self.conns[c.ip] -= 1
            if self.conns[c.ip] <= 0: del self.conns[c.ip]

    async def host(self, c, build, want):
        if len(self.rooms) >= MAX_ROOMS or self.hosted[c.ip] >= PER_IP_ROOMS:
            c.send(ERROR, 0, b'relay full, try again later'); return
        if want != '*' and (len(want) != CODE_LEN or any(ch not in ALPHABET for ch in want)):
            c.send(ERROR, 0, b'bad room code'); return
        if want != '*' and self.refused(c.ip): c.send(ERROR, 0, b'too many wrong codes, wait a minute'); return
        if want in self.rooms:                   # tells that the code exists: counted like a wrong join code
            self.bad[c.ip].append(time.monotonic()); c.send(ERROR, 0, b'room code in use'); return
        room = Room(self.new_code() if want == '*' else want, c, build); self.rooms[room.code] = room; self.hosted[c.ip] += 1
        log.info('%s: hosting %s', c.ip, room.code)
        c.send(ROOM, 0, room.code.encode())
        try:
            while True:
                kind, peer, body = await asyncio.wait_for(read_frame(c.reader), IDLE)
                j = room.peers.get(peer)
                if kind == DATA and j: j.send(DATA, 0, body)
                elif kind == CLOSE and j: j.send(CLOSE); j.close(); room.peers.pop(peer, None)
                elif kind == PING: c.send(PONG, peer, body)
        finally:
            del self.rooms[room.code]
            self.hosted[c.ip] -= 1
            if self.hosted[c.ip] <= 0: del self.hosted[c.ip]
            for j in room.peers.values(): j.send(CLOSE); j.close()
            room.peers.clear()                   # a joiner leaving late has no host to tell
            log.info('%s: room %s closed', c.ip, room.code)

    async def join(self, c, code, build):
        if self.refused(c.ip): c.send(ERROR, 0, b'too many wrong codes, wait a minute'); return
        room = self.rooms.get(code)
        if not room: self.bad[c.ip].append(time.monotonic()); c.send(ERROR, 0, b'no such room'); return
        if build != room.build: c.send(ERROR, 0, b'version: the host runs another build of the game'); return
        peer = room.free_peer()
        if not peer: c.send(ERROR, 0, b'room full'); return
        room.peers[peer] = c
        log.info('%s: joined %s as %d', c.ip, code, peer)
        room.host.send(OPEN, peer); c.send(OPEN)
        try:
            while not c.closed:
                kind, _, body = await asyncio.wait_for(read_frame(c.reader), IDLE)
                if kind == DATA: room.host.send(DATA, peer, body)
                elif kind == CLOSE: break
                elif kind == PING: c.send(PONG, 0, body)
        finally:
            if room.peers.get(peer) is c:
                del room.peers[peer]
                room.host.send(CLOSE, peer)


async def main(argv=None):
    ap = argparse.ArgumentParser(description='Rainbow Six co-op relay')
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, action='append', help='TCP port to listen on (repeatable; default 2346)')
    ap.add_argument('--proxy-protocol', action='store_true', help="each connection starts with a PROXY v1 header (behind Fly.io's proxy_proto)")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    relay = Relay(proxy_protocol=a.proxy_protocol)
    servers = [await asyncio.start_server(relay.serve, a.host, p) for p in (a.port or [2346])]
    for s in servers:
        for sock in s.sockets: log.info('listening on %s:%d', *sock.getsockname()[:2])
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == '__main__':
    try: asyncio.run(main())
    except KeyboardInterrupt: pass
