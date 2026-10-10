#!/usr/bin/env python3
"""FGUY Mod lobby + relay server (Family Guy: Back to the Multiverse online play).

One small program, standard library only (Python 3.8+). It does two jobs on one port number:

  TCP  lobby: players host / list / join lobbies, see who's in, get ready; the host starts the game.
  UDP  relay: during a game every player's packets go through here to the other players, so nobody
       has to open ports on their router.

Run:   python3 fguy_lobby.py [--port 7650]
Then open the port for TCP and UDP in the firewall (see server/README.md).

Lobby protocol: UTF-8 text lines, fields separated by single spaces.
  client -> server
    HELLO <proto> <gamekey> <name>   first line. gamekey identifies the game version (all players
                                     in a lobby must match). name: letters, digits, _ -, max 16.
    LIST                             open lobbies
    HOST <maxplayers 2-4> [<private 0|1>]   create a lobby (you get slot 0); private = join by code only
    JOIN <code>                      join a lobby by its code
    READY <0|1>
    SET <character> <costume> <team> your picks (character name, costume index, team 0 good / 1 evil)
    CONF <mode> <map> <time> <score> <ff>   host: match settings (short codes / list indices)
    RTT <ms>                         your measured round trip to this server
    PING <x>                         answered with PONG <x>
    SAVE <base64>                    host: the save file every player starts the session from
    LANG <code>                      host: game language for the session
    START                            host: start the session
    LEAVE
  server -> client
    WELCOME <proto>
    LIST [<code>,<host>,<players>,<max>,<mode>,<map> ...]     (public lobbies)
    LOBBY <code> <private>
    MEMBERS <your slot> [<slot>,<name>,<ready>,<rtt>,<character>,<costume>,<team> ...]
    CONF <mode> <map> <time> <score> <ff>
    SAVE <base64>                    sent right before START when the host shared a save
    START <token> <slot> <players> <input delay> <udp port> <lang> <conf> <picks>
                                     conf = mode,map,time,score,ff; picks = char:costume:team;... by slot
                                     (a snapshot, so every player starts from exactly the same choices)
    CLOSED <reason...>               the lobby is gone (host left)
    ERROR <text...>
    PONG <x>

Relay protocol (UDP): 'F' 'G' <8-byte session token> <slot> <payload>. The server remembers each
slot's address from its packets and forwards every packet unchanged to the session's other players.
"""
import argparse
import asyncio
import base64
import logging
import math
import os
import re
import secrets
import time

PROTO = 2
# No I/O/0/1 (look alike), and no A/L/R/X/Y: the game turns the controller's A, LB, RB, X and Y buttons into
# those letter keys, so typing a code on a keyboard must never need them.
CODE_LETTERS = 'BCDEFGHJKMNPQSTUVWZ23456789'
NAME_RE = re.compile(r'^[A-Za-z0-9_\-]{1,16}$')
TOKEN_RE = re.compile(r'^[A-Za-z0-9_\-]{1,24}$')   # characters, modes, maps, numbers
MAX_LINE = 2 * 1024 * 1024        # a save file (~450 KB) as base64 fits easily
MAX_SAVE = 1024 * 1024
MAX_CLIENTS_PER_IP = 8
MAX_LOBBIES = 500
SESSION_IDLE_S = 120              # forget a game session after this long without packets
RELAY_MAX_PPS = 2000              # per player address
TICK_MS = 1000 / 60

log = logging.getLogger('fguy')


class Client:
    def __init__(self, server, reader, writer):
        self.server, self.reader, self.writer = server, reader, writer
        self.ip = writer.get_extra_info('peername')[0]
        self.name = None
        self.gamekey = None
        self.lobby = None
        self.ready = False
        self.rtt = 0
        self.char, self.costume, self.team = 'Peter', '0', '0'

    def send(self, line):
        try:
            self.writer.write((line + '\n').encode())
        except Exception:
            pass

    @property
    def slot(self):
        return self.lobby.members.index(self) if self.lobby else -1


class Lobby:
    def __init__(self, code, host, max_players, private):
        self.code, self.max, self.private = code, max_players, private
        self.members = [host]
        self.save = None
        self.lang = '-'
        self.conf = ['DM', 'FR', '0', '0', '0']  # mode, map, time, score, friendly fire

    @property
    def host(self):
        return self.members[0]

    def broadcast_members(self):
        entries = ' '.join(f'{i},{c.name},{int(c.ready)},{c.rtt},{c.char},{c.costume},{c.team}'
                           for i, c in enumerate(self.members))
        for c in self.members:
            c.send(f'MEMBERS {c.slot} {entries}'.rstrip())

    def broadcast_conf(self, only=None):
        for c in ([only] if only else self.members):
            c.send('CONF ' + ' '.join(self.conf))


class Session:
    def __init__(self, players):
        self.addrs = [None] * players
        self.last = time.monotonic()


class Server:
    def __init__(self, port):
        self.port = port
        self.clients = set()
        self.lobbies = {}
        self.sessions = {}
        self.transport = None
        self.rate = {}  # addr -> [window start, count]

    # ---- lobby (TCP) -------------------------------------------------------------------------------
    async def handle(self, reader, writer):
        c = Client(self, reader, writer)
        if sum(1 for o in self.clients if o.ip == c.ip) >= MAX_CLIENTS_PER_IP:
            writer.close()
            return
        self.clients.add(c)
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    self.command(c, line.decode('utf-8', 'replace').strip())
                except Exception as e:  # a bad line must never take the server down
                    log.exception('command failed: %s', e)
                    c.send('ERROR bad request')
                await writer.drain()
        except (asyncio.LimitOverrunError, ValueError, ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            self.leave(c)
            self.clients.discard(c)
            writer.close()

    def command(self, c, line):
        parts = line.split(' ')
        cmd, args = parts[0].upper(), parts[1:]
        if cmd == 'PING':
            c.send('PONG ' + (args[0] if args else ''))
            return
        if cmd == 'HELLO':
            if len(args) < 3 or not args[0].isdigit() or int(args[0]) != PROTO:
                c.send('ERROR Please update the mod (server protocol %d)' % PROTO)
                return
            if not NAME_RE.match(args[2]):
                c.send('ERROR Names can use letters, digits, _ and -, up to 16')
                return
            c.gamekey, c.name = args[1][:32], args[2]
            c.send(f'WELCOME {PROTO}')
            log.info('%s connected from %s', c.name, c.ip)
            return
        if not c.name:
            c.send('ERROR say HELLO first')
            return
        if cmd == 'LIST':
            open_ = [l for l in self.lobbies.values()
                     if l.host.gamekey == c.gamekey and len(l.members) < l.max and not l.private]
            c.send(' '.join(['LIST'] + [f'{l.code},{l.host.name},{len(l.members)},{l.max},{l.conf[0]},{l.conf[1]}'
                                        for l in open_[:50]]))
        elif cmd == 'HOST':
            self.leave(c)
            if len(self.lobbies) >= MAX_LOBBIES:
                c.send('ERROR The server is full, try again later')
                return
            n = int(args[0]) if args and args[0].isdigit() else 4
            private = len(args) > 1 and args[1] == '1'
            code = self.new_code()
            c.lobby, c.ready = Lobby(code, c, max(2, min(4, n)), private), False
            self.lobbies[code] = c.lobby
            c.send(f'LOBBY {code} {int(private)}')
            c.lobby.broadcast_members()
            c.lobby.broadcast_conf()
            log.info('%s hosts lobby %s', c.name, code)
        elif cmd == 'JOIN':
            code = args[0].upper() if args else ''
            lobby = self.lobbies.get(code)
            if not lobby:
                c.send('ERROR No lobby with that code')
            elif lobby is c.lobby:
                pass
            elif lobby.host.gamekey != c.gamekey:
                c.send('ERROR That lobby runs a different version of the game or mod')
            elif len(lobby.members) >= lobby.max:
                c.send('ERROR That lobby is full')
            else:
                self.leave(c)
                c.lobby, c.ready = lobby, False
                lobby.members.append(c)
                c.send(f'LOBBY {code} {int(lobby.private)}')
                lobby.broadcast_members()
                lobby.broadcast_conf(only=c)
                log.info('%s joins lobby %s', c.name, code)
        elif cmd == 'READY' and c.lobby:
            c.ready = bool(args and args[0] == '1')
            c.lobby.broadcast_members()
        elif cmd == 'SET' and c.lobby:
            if len(args) == 3 and all(TOKEN_RE.match(a) for a in args):
                c.char, c.costume, c.team = args
                c.lobby.broadcast_members()
        elif cmd == 'CONF' and c.lobby and c is c.lobby.host:
            if len(args) == 5 and all(TOKEN_RE.match(a) for a in args):
                c.lobby.conf = list(args)
                c.lobby.broadcast_conf()
        elif cmd == 'RTT':
            c.rtt = max(0, min(2000, int(args[0]))) if args and args[0].isdigit() else 0
            if c.lobby:
                c.lobby.broadcast_members()
        elif cmd == 'SAVE' and c.lobby and c is c.lobby.host:
            data = base64.b64decode(args[0]) if args else b''
            c.lobby.save = args[0] if 0 < len(data) <= MAX_SAVE else None
        elif cmd == 'LANG' and c.lobby and c is c.lobby.host:
            c.lobby.lang = re.sub(r'[^A-Za-z_]', '', args[0])[:8] if args else '-'
        elif cmd == 'START' and c.lobby and c is c.lobby.host:
            self.start(c.lobby)
        elif cmd == 'LEAVE':
            self.leave(c)
        else:
            c.send('ERROR unknown command')

    def new_code(self):
        while True:
            code = ''.join(secrets.choice(CODE_LETTERS) for _ in range(4))
            if code not in self.lobbies:
                return code

    def leave(self, c):
        lobby = c.lobby
        if not lobby:
            return
        c.lobby = None
        if lobby.host is c:  # the host leaving closes the lobby
            self.lobbies.pop(lobby.code, None)
            for m in lobby.members[1:]:
                m.lobby = None
                m.send('CLOSED The host left the lobby')
            log.info('lobby %s closed', lobby.code)
        else:
            lobby.members.remove(c)
            lobby.broadcast_members()

    def start(self, lobby):
        if len(lobby.members) < 2:
            lobby.host.send('ERROR You need at least one other player')
            return
        if not all(m.ready for m in lobby.members[1:]):
            lobby.host.send('ERROR Not everyone is ready')
            return
        token = secrets.token_bytes(8)
        n = len(lobby.members)
        self.sessions[token] = Session(n)
        # Inputs for tick T are sent when tick T-delay+1 starts and must arrive before tick T starts.
        # Through the relay a packet takes at most about (slowest player's round trip) to arrive.
        worst = max(m.rtt for m in lobby.members)
        delay = max(2, min(15, 1 + math.ceil((worst + 15) / TICK_MS)))
        conf = ','.join(lobby.conf)
        picks = ';'.join(f'{m.char}:{m.costume}:{m.team}' for m in lobby.members)
        for i, m in enumerate(lobby.members):
            if lobby.save and i > 0:
                m.send('SAVE ' + lobby.save)
            m.send(f'START {token.hex()} {i} {n} {delay} {self.port} {lobby.lang} {conf} {picks}')
            m.lobby = None
        self.lobbies.pop(lobby.code, None)
        log.info('lobby %s started: %d players, input delay %d (worst rtt %d ms)', lobby.code, n, delay, worst)

    # ---- relay (UDP) -------------------------------------------------------------------------------
    def datagram(self, data, addr):
        if len(data) < 11 or data[0:2] != b'FG' or len(data) > 1400:
            return
        now = time.monotonic()
        r = self.rate.get(addr)
        if not r or now - r[0] >= 1:
            r = self.rate[addr] = [now, 0]
        r[1] += 1
        if r[1] > RELAY_MAX_PPS:
            return
        s = self.sessions.get(data[2:10])
        slot = data[10]
        if not s or slot >= len(s.addrs):
            return
        s.addrs[slot] = addr
        s.last = now
        for i, a in enumerate(s.addrs):
            if a and i != slot:
                self.transport.sendto(data, a)

    async def cleanup(self):
        while True:
            await asyncio.sleep(30)
            now = time.monotonic()
            for t in [t for t, s in self.sessions.items() if now - s.last > SESSION_IDLE_S]:
                del self.sessions[t]
            self.rate = {a: r for a, r in self.rate.items() if now - r[0] < 5}


class Relay(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server

    def connection_made(self, transport):
        self.server.transport = transport

    def datagram_received(self, data, addr):
        self.server.datagram(data, addr)


async def main():
    ap = argparse.ArgumentParser(description='FGUY Mod lobby + relay server')
    ap.add_argument('--port', type=int, default=int(os.environ.get('FGUY_PORT', 7650)))
    ap.add_argument('--host', default='0.0.0.0')
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    server = Server(a.port)
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: Relay(server), local_addr=(a.host, a.port))
    tcp = await asyncio.start_server(server.handle, a.host, a.port, limit=MAX_LINE)
    asyncio.ensure_future(server.cleanup())
    log.info('FGUY lobby server on port %d (TCP lobby + UDP relay)', a.port)
    async with tcp:
        await tcp.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
