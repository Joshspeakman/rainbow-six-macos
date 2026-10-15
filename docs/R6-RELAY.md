# Rainbow Six internet co-op through a relay

**Status: in development (branch `relay`).** Co-op M01 through a local relay, driven by script
(`tools/r6_relay_local.py --drive`), 2026-10-15:
- Windows: a host and two joiners on one machine reached one lobby and the mission. Joiners arrived from 127.0.0.2 and
  127.0.0.3.
- macOS (rainbow-six-macos, branch `relay`): a host and one joiner; joiner movement shows on the host.

Not yet checked: a relay on the internet, players on different machines, the launcher rows, and two joiners on a Mac
host (both arrive from 127.0.0.1 there).

**What the game uses with `MultiplayerBehindFirewall=TRUE`** (W32_NETLOG):
- Each joiner opens exactly one TCP connection, to the host's join port; the host never connects back.
- The game's UDP sockets (2347, 2348 and an ephemeral one) carry only LAN announcements and the server browser. In
  relay mode they stay on loopback, and co-op needs none of them. A broadcast sendto fails with 10049 on macOS (it
  succeeds on Windows), and a second game on one machine fails to bind 2347 (10048); the game ignores both.

Menus, in 640x480 game coordinates for `DS_CMDFILE`: main menu Multiplayer (192,256). Multiplayer screen: Create Game
(90,126), Manual Join (90,158), and M Player (90,222), which starts mplaynow.exe. Avoid it. Lobby: Cooperative
(242,128), which gives M01, then Start Mission (535,383). Manual Join is pre-filled from the registry; Join is at
(321,295).

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
| `R6_RELAY_CODE` | Host only: the room code to ask for. The launcher picks it and keeps it, so it can show the code before the game starts and friends can reuse it. Without it the relay picks a free code |
| `R6_RELAY_NAME` | The host's player name, shown in the relay's log only |
| `R6_RELAY_BUILD` | The first 16 hex digits (lowercase) of the SHA-256 of the mod's **RainbowSix.exe**, the game executable, never the platform's runtime binary (RainbowSixWindows.exe, the Mac app), which differs per platform: every launcher must hash the same file, or Mac and PC players never match. A joiner whose build differs from the host's is refused |
| `R6_RELAY_LOCAL_PORT` | Joiner only: the local listener's port (2356 by default, not 2346, which the joining game may bind itself). Different ports let host and joiners share one machine for tests |
| `R6_RELAY_GAME_PORT` | The game's join port, `MultiplayerGameJoinPort` (2346 by default). Only a host's `listen()` on this port opens a room |
| `R6_RELAY_STATUS` | A file the runtime rewrites with `key=value` lines: `state` (connecting, hosting, ready, joined, error, idle), `room`, `players` (host), `message` (error) |

The launcher must also force these game options in relay mode, on every player:
`MultiplayerBehindFirewall=TRUE` (everything over the one TCP connection), `R6CoopOnline=FALSE` (the mod's DHT list
uses UDP), `R6CoopRouter=FALSE` (no UPnP), and `MultiplayerManualHostAddress=127.0.0.1` with
`MultiplayerManualJoinPort` set to the local port (joiners).

## Launcher: Play Online

The launcher adds its own rows to the mod's `r6coop-launcher.json` at start-up, so the mod's file is unchanged. The
logic is in `recomp/host/linux_launcher/r6_online.py`, with tests in `recomp/tests/r6_online_test.py`; the macOS
launcher does the same.

| Row | Where | Registry value | Shown |
|---|---|---|---|
| Play Online: Off / Host a Room / Join a Room | top, after Controls | `R6RelayMode` (`off`, `host`, `join`) | always |
| Your Room Code | top | `R6RelayHostCode` | Host. Made once (e.g. `K7Q-M4X`) and kept; the player may type another valid code |
| Room Code | top | `R6RelayJoinCode` | Join |
| Relay | Multiplayer | `R6RelayServer`, default `r6coop-relay.fly.dev:2346` | always |

A row's `"visible_if": {"online_mode": "host"}` shows it only in that mode.

**Play** with Host or Join:
1. **Checks the code:** 6 characters from the alphabet; case, dashes and spaces don't matter. Anything else gets a
   message, and the game doesn't start.
2. **Sets the runtime settings:**
   - `R6_RELAY` is the Relay row;
   - `R6_RELAY_BUILD` is the first 16 hex digits of the SHA-256 of the mod's `RainbowSix.exe`;
   - `R6_RELAY_NAME` is the player name;
   - `R6_RELAY_STATUS` is `<data>/relay-status.txt`;
   - the host gets `R6_ROOM=host` and `R6_RELAY_CODE=<code>`; a joiner gets `R6_ROOM=<code>` and
     `R6_RELAY_LOCAL_PORT=2356`.
3. **Forces the game options relay play needs:** `MultiplayerBehindFirewall=TRUE`, `R6CoopOnline=FALSE` and
   `R6CoopRouter=FALSE`; a joiner also gets `MultiplayerManualHostAddress=127.0.0.1` and
   `MultiplayerManualJoinPort=2356`. The player's own values are kept first, as `R6RelaySaved.<name>`.

Play with Off puts the kept values back, removes the kept copies, and clears every `R6_RELAY*` variable. In the
game, the host uses Multiplayer, Create Game; a joiner uses Multiplayer, Manual Join, Join (already filled in).

`DS_LAUNCHER_PLAY=1` presses Play at once, for driven tests. Verified 2026-10-15: a host and a joiner, each started
this way on Windows, met in room 79J3TB on the Fly relay and reached M01; with Off, the player's own values came back.

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
| 01 | HELLO | client → relay | `R6R1 host <build> <CODE or *> <name>` or `R6R1 join <CODE> <build>`. The first frame on a connection, within 10 s. A host's `*` lets the relay pick the code; a code already open gets `room code in use` |
| 02 | ROOM | relay → host | The room's code |
| 03 | OPEN | relay → host | Joiner `<peer>` (1-15) arrived |
| 03 | OPEN | relay → joiner | peer 0: you are in the room |
| 04 | DATA | both ways | Bytes of the joiner's game connection. The host side carries the joiner's peer number; the joiner side carries peer 0 |
| 05 | CLOSE | both ways | That joiner's connection ended. To a joiner it means the host closed it; the relay then hangs up |
| 06 | PING | either side | Answered with 07 PONG carrying the same payload. Clients ping every 15 s |
| 07 | PONG | either side | |
| 08 | ERROR | relay → client | Text: `no such room`, `room full`, `room code in use`, `bad room code`, `version: ...`, `too many wrong codes, wait a minute`, `relay full, try again later`. The relay then hangs up |

Room codes are 6 characters from `BCDFGHJKMPQRTVWXY2346789` (no vowels, and no 0/O, 1/I/L or 5/S).

Relay limits: 15 joiners per room. A connection silent for 60 s is dropped. A connection whose unsent data passes
256 KiB is dropped; a dropped joiner reaches the host as CLOSE, and a dropped host ends the room. Per address: 32
connections and 4 hosted rooms. 20 wrong codes in a minute from one address refuse that address for a minute. A host's `room code in use` counts as a wrong code, since it shows that the code exists.
These per-address limits count a public IP, so players behind one router (a LAN party) share them.

## Running the relay

```
python3 recomp/relay/r6relay.py --port 2346 --port 443
```

It needs a machine reachable from the internet; that is the only thing anyone sets up, once. Players' firewalls and
routers need nothing.

### On Fly.io

`recomp/relay` has the `Dockerfile` and `fly.toml`. Fly builds the image remotely, so no local Docker is needed.

- **Install and log in:** install flyctl (`winget install Fly-io.flyctl` on Windows), then `flyctl auth login`.
- **Deploy**, from `recomp/relay`:
  ```
  flyctl launch --copy-config --no-deploy     # once: creates the app (the name in fly.toml must be free on Fly)
  flyctl deploy --ha=false                    # exactly one machine
  flyctl ips allocate-v4                      # once: a dedicated IPv4 (billed monthly); plain TCP needs one
  ```
- **Address:** players use `<app>.fly.dev:2346`, or port 443 on networks that only allow web ports.
- **Exactly one machine, never stopped:** rooms live in the relay's memory. Two machines would split rooms between
  them, and a stopped machine ends every room. `fly.toml` sets `auto_stop_machines = "off"`, and `--ha=false` keeps
  it to one machine. `flyctl scale count 1` puts it back to one if needed.
- **Real player addresses:** Fly's `proxy_proto` handler sends each connection's real address in a PROXY v1 header.
  The container runs the relay with `--proxy-protocol` to read it. Without that, every player would seem to come from
  Fly's proxy and share one address's limits.
- **Logs:** `flyctl logs` shows rooms opening and closing, and the address of each player who joins.

## Tests

```
cd recomp/relay && python3 -m unittest test_r6relay
cd recomp && clang -O1 -g -D_GNU_SOURCE -I runtime/win_include -o relay_test.exe tests/relay_test.c -pthread -lws2_32 -liphlpapi
python3 tests/relay_e2e.py ./relay_test.exe
```

On Linux and macOS, build with `cc -O1 -g -o relay_test tests/relay_test.c -pthread`.
