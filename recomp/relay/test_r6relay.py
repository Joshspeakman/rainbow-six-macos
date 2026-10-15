"""r6relay.py against real loopback sockets: rooms, both directions of DATA, CLOSE either way, errors and limits.
Run: python -m unittest test_r6relay (from this folder)."""
import asyncio
import struct
import unittest

import r6relay as R


BUILD = '4175039d2a57c4a7'
OPEN_CLIENTS = []


class Client:
    @classmethod
    async def open(cls, port, hello):
        self = cls(); self.r, self.w = await asyncio.open_connection('127.0.0.1', port)
        OPEN_CLIENTS.append(self)
        self.send(R.HELLO, 0, hello.encode()); return self

    def send(self, kind, peer=0, body=b''):
        self.w.write(R.frame(kind, peer, body))

    async def recv(self, timeout=2):
        return await asyncio.wait_for(R.read_frame(self.r), timeout)

    async def eof(self, timeout=2):
        try: return await asyncio.wait_for(self.r.read(1), timeout) == b''
        except ConnectionError: return True


class RelayTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.relay = R.Relay()
        self.server = await asyncio.start_server(self.relay.serve, '127.0.0.1', 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        for c in OPEN_CLIENTS: c.w.close()
        OPEN_CLIENTS.clear()
        self.server.close(); await self.server.wait_closed()

    async def hosted(self):
        h = await Client.open(self.port, f'R6R1 host JOHN DOE {BUILD}')
        kind, _, code = await h.recv()
        self.assertEqual(kind, R.ROOM); self.assertEqual(len(code), R.CODE_LEN)
        return h, code.decode()

    async def joined(self, code, build=BUILD):
        j = await Client.open(self.port, f'R6R1 join {code} {build}')
        self.assertEqual((await j.recv())[:2], (R.OPEN, 0))
        return j

    async def test_bytes_both_ways(self):
        h, code = await self.hosted()
        j = await self.joined(code[:3] + '-' + code[3:].lower())       # dashes and case don't matter
        kind, peer, _ = await h.recv(); self.assertEqual((kind, peer), (R.OPEN, 1))
        j.send(R.DATA, 0, b'hello host')
        self.assertEqual(await h.recv(), (R.DATA, 1, b'hello host'))
        h.send(R.DATA, 1, b'x' * R.MAX_PAYLOAD)
        self.assertEqual(await j.recv(), (R.DATA, 0, b'x' * R.MAX_PAYLOAD))
        j.send(R.DATA, 0, b'')                                           # an empty DATA is legal
        self.assertEqual(await h.recv(), (R.DATA, 1, b''))

    async def test_two_joiners_are_separate(self):
        h, code = await self.hosted()
        a = await self.joined(code); self.assertEqual((await h.recv())[:2], (R.OPEN, 1))
        b = await self.joined(code); self.assertEqual((await h.recv())[:2], (R.OPEN, 2))
        b.send(R.DATA, 0, b'from b'); self.assertEqual(await h.recv(), (R.DATA, 2, b'from b'))
        h.send(R.DATA, 1, b'to a'); self.assertEqual(await a.recv(), (R.DATA, 0, b'to a'))
        h.send(R.DATA, 2, b'to b'); self.assertEqual(await b.recv(), (R.DATA, 0, b'to b'))

    async def test_joiner_leaving_tells_host(self):
        h, code = await self.hosted()
        j = await self.joined(code); await h.recv()
        j.w.close()
        self.assertEqual((await h.recv())[:2], (R.CLOSE, 1))
        j2 = await self.joined(code)                                     # the peer number is free again
        self.assertEqual((await h.recv())[:2], (R.OPEN, 1))

    async def test_host_closing_a_joiner(self):
        h, code = await self.hosted()
        j = await self.joined(code); await h.recv()
        h.send(R.CLOSE, 1)
        self.assertEqual((await j.recv())[:2], (R.CLOSE, 0))
        self.assertTrue(await j.eof())

    async def test_host_leaving_ends_the_room(self):
        h, code = await self.hosted()
        j = await self.joined(code); await h.recv()
        h.w.close()
        self.assertEqual((await j.recv())[:2], (R.CLOSE, 0))
        self.assertTrue(await j.eof())
        await asyncio.sleep(0.05)
        self.assertNotIn(code, self.relay.rooms)
        late = await Client.open(self.port, f'R6R1 join {code} {BUILD}')
        self.assertEqual(await late.recv(), (R.ERROR, 0, b'no such room'))

    async def test_ping(self):
        h, code = await self.hosted()
        h.send(R.PING, 0, b'42'); self.assertEqual(await h.recv(), (R.PONG, 0, b'42'))
        j = await self.joined(code); await h.recv()
        j.send(R.PING, 0, b'7'); self.assertEqual(await j.recv(), (R.PONG, 0, b'7'))

    async def test_wrong_version_and_hello(self):
        c = await Client.open(self.port, f'R6R9 host X {BUILD}')
        kind, _, msg = await c.recv(); self.assertEqual(kind, R.ERROR); self.assertIn(b'version', msg)
        c = await Client.open(self.port, 'R6R1 dance')
        self.assertEqual((await c.recv())[0], R.ERROR)

    async def test_other_build_is_refused(self):
        h, code = await self.hosted()
        c = await Client.open(self.port, f'R6R1 join {code} 0123456789abcdef')
        kind, _, msg = await c.recv(); self.assertEqual(kind, R.ERROR); self.assertIn(b'version', msg)
        self.assertTrue(await c.eof())

    async def test_oversized_frame_drops_the_sender(self):
        h, code = await self.hosted()
        j = await self.joined(code); await h.recv()
        h.w.write(R.struct.pack('>BBH', R.DATA, 1, R.MAX_PAYLOAD + 1) + b'x' * (R.MAX_PAYLOAD + 1))
        self.assertTrue(await h.eof())
        self.assertEqual((await j.recv())[:2], (R.CLOSE, 0))

    async def test_room_full(self):
        h, code = await self.hosted()
        for _ in range(R.MAX_PEERS): await self.joined(code)
        c = await Client.open(self.port, f'R6R1 join {code} {BUILD}')
        self.assertEqual(await c.recv(), (R.ERROR, 0, b'room full'))

    async def test_guessing_codes_is_refused(self):
        h, code = await self.hosted()
        for _ in range(R.BAD_JOINS_PER_MIN):
            c = await Client.open(self.port, f'R6R1 join BBBBBB {BUILD}'); await c.recv()
        c = await Client.open(self.port, f'R6R1 join {code} {BUILD}')                # even the right code, for a minute
        kind, _, msg = await c.recv(); self.assertEqual(kind, R.ERROR); self.assertIn(b'wrong codes', msg)

    async def test_silent_hello_times_out(self):
        R.HELLO_TIMEOUT, old = 0.2, R.HELLO_TIMEOUT
        try:
            r, w = await asyncio.open_connection('127.0.0.1', self.port)
            self.assertEqual(await asyncio.wait_for(r.read(1), 2), b'')
            w.close()
        finally: R.HELLO_TIMEOUT = old


if __name__ == '__main__':
    unittest.main()
