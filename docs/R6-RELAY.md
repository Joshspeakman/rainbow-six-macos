# Rainbow Six internet co-op through a relay

**Status: in development (branch `relay`).** The relay server and the runtime bridge pass their tests without the game.
Not yet checked: the game itself joining through it, multi-joiner co-op, and the Mac port.

## Why

The game's host waits for incoming connections (TCP 2346, UDP 2347-2348), which firewalls and home routers block.
In relay mode every connection is **outgoing**: each player's game connects to one relay server, which carries the bytes
between a room's host and its joiners. Nobody opens a port, adds a firewall rule, maps a router port (UPnP), runs a
VPN or needs admin rights, and nothing is left behind when the game closes. The game itself only ever listens on
127.0.0.1, so Windows never shows its firewall prompt.

## Pieces

| Piece | Where | What |
|---|---|---|
| Relay server | `recomp/relay/r6relay.py` | Python 3 standard library, plain TCP. Rooms with 6-character codes. Tests: `recomp/relay/test_r6relay.py` |
| Runtime bridge | `recomp/runtime/win32/relay.c` | The game-side end. Hooks in `winsock.c` (bind, connect, listen, closesocket) and `net_ole.c` (WSAStartup, the host address) |
| Bridge test | `recomp/tests/relay_test.c`, `recomp/tests/relay_e2e.py` | The bridge against the real relay, with stand-ins for the game |

## Runtime settings (the launcher sets them)

| Variable | Meaning |
|---|---|
| `R6_RELAY` | Relay address, `host[:port]` (port 2346 by default). Relay mode is on when this and `R6_ROOM` are both set |
| `R6_ROOM` | `host` to host, or the room code to join (case and dashes don't matter) |
| `R6_RELAY_NAME` | The host's player name, shown in the relay's log only |
| `R6_RELAY_BUILD` | The first 16 hex digits of the game executable's SHA-256. A joiner whose build differs from the host's is refused |
| `R6_RELAY_LOCAL_PORT` | Joiner only: the local listener's port (2356 by default, not 2346, which the joining game may bind itself). Different ports let host and joiners share one machine for tests |
| `R6_RELAY_GAME_PORT` | The game's join port, `MultiplayerGameJoinPort` (2346 by default). Only a host's `listen()` on this port opens a room |
| `R6_RELAY_STATUS` | A file the runtime rewrites with `key=value` lines: `state` (connecting, hosting, ready, joined, error, idle), `room`, `players` (host), `message` (error) |

The launcher must also force these game options in relay mode, on every player:
`MultiplayerBehindFirewall=TRUE` (everything over the one TCP connection), `R6CoopOnline=FALSE` (the mod's DHT list
uses UDP), `R6CoopRouter=FALSE` (no UPnP), and `MultiplayerManualHostAddress=127.0.0.1` with
`MultiplayerManualJoinPort` set to the local port (joiners).

## What the bridge does

- **Every guest `bind()`** to an address other than 127.x is made to 127.0.0.1, port kept. With the relay on,
  `gethostbyname` of this machine also lists 127.0.0.1 first, the address Rainbow Six hosts on.
- **Host:** when the game calls `listen()` on its join port, a thread connects to the relay, sends `host`, and receives the room code
  (into the status file). For each joiner the relay announces, the thread connects to the game's port from
  127.0.0.(1 + joiner number), so the game sees each joiner at a different address. macOS has only 127.0.0.1 without
  an alias, so there every joiner comes from 127.0.0.1. When the game closes that listening socket, the room ends.
- **Joiner:** from the first `WSAStartup`, a thread listens on 127.0.0.1:`R6_RELAY_LOCAL_PORT`. Each connection the
  game makes there becomes one join to the room. A game `connect()` to the join port on any non-loopback address is sent
  to the local listener too, so an address typed by hand still goes through the relay.
- **Pumps never block on a send.** Each connection has a queue. A side whose queue passes 256 KiB isn't read until it
  drains, so one thread can carry both directions without deadlocking. Every socket has TCP_NODELAY.

## Wire protocol (R6R1)

TCP. Every message is a frame: `[u8 type][u8 peer][u16 length, big-endian][payload]`. The payload is at most
**4096** bytes; senders split larger reads, and an empty `DATA` is legal.

| Type | Name | Direction | Payload and meaning |
|---|---|---|---|
| 01 | HELLO | client → relay | `R6R1 host <name> <build>` or `R6R1 join <CODE> <build>`. The first frame on a connection, within 10 s |
| 02 | ROOM | relay → host | The room's code |
| 03 | OPEN | relay → host | Joiner `<peer>` (1-15) arrived |
| 03 | OPEN | relay → joiner | peer 0: you are in the room |
| 04 | DATA | both ways | Bytes of the joiner's game connection. The host side carries the joiner's peer number; the joiner side carries peer 0 |
| 05 | CLOSE | both ways | That joiner's connection ended. To a joiner it means the host closed it; the relay then hangs up |
| 06 | PING | either side | Answered with 07 PONG carrying the same payload. Clients ping every 15 s |
| 07 | PONG | either side | |
| 08 | ERROR | relay → client | Text: `no such room`, `room full`, `version: ...`, `too many wrong codes, wait a minute`, `relay full, try again later`. The relay then hangs up |

Room codes are 6 characters from `BCDFGHJKMPQRTVWXY2346789` (no vowels, and no 0/O, 1/I/L or 5/S).

Relay limits: 15 joiners per room. A connection silent for 60 s is dropped. A connection whose unsent data passes
256 KiB is dropped; a dropped joiner reaches the host as CLOSE, and a dropped host ends the room. Per address: 32
connections and 4 hosted rooms. 20 wrong codes in a minute from one address refuse that address for a minute.
These per-address limits count a public IP, so players behind one router (a LAN party) share them.

## Running the relay

```
python3 recomp/relay/r6relay.py --port 2346 --port 443
```

It needs a machine reachable from the internet; that is the only thing anyone sets up, once. Players' firewalls and
routers need nothing.

## Tests

```
cd recomp/relay && python3 -m unittest test_r6relay
cd recomp && clang -O1 -g -D_GNU_SOURCE -I runtime/win_include -o relay_test.exe tests/relay_test.c -pthread -lws2_32 -liphlpapi
python3 tests/relay_e2e.py ./relay_test.exe
```

On Linux and macOS, build with `cc -O1 -g -o relay_test tests/relay_test.c -pthread`.
