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
    SET <character> <costume> <team> [<input>]   your picks (character name, costume index, team 0 good /
                                     1 evil; input k = keyboard and mouse, c = controller)
    CONF <mode> <map> <time> <score> <ff> [<rate>]   host: match settings (short codes / list indices;
                                     rate: frames per second, 0 = auto)
    RATE <hz>                        the most frames per second this player's monitor shows (60 / 120 / 144)
    OWN <list>                       host: what the host's save has (the session plays on it), so everyone
                                     picks from it: <char>:<costume>.<costume>...;<char>:...
    RTT <ms>                         your measured round trip to this server
    PING <x>                         answered with PONG <x>
    SAVE <base64>                    host: the save file every player starts the session from
    LANG <code>                      host: game language for the session
    START                            host: start the session
    REJOIN <code> <token> <slot>     back from a match: take my place in that lobby again (the lobby
                                     stays while its match runs; its players show as "in match")
    LEAVE
  server -> client
    WELCOME <proto>
    LIST [<code>,<host>,<players>,<max>,<mode>,<map> ...]     (public lobbies)
    LOBBY <code> <private>
    MEMBERS <your slot> [<slot>,<name>,<ready>,<rtt>,<character>,<costume>,<team>,<input>,<rate> ...]
                                     ready: 0 / 1, or 2 = still in the match
    CONF <mode> <map> <time> <score> <ff>
    OWN <list>                       the host's characters and costumes (also sent on joining)
    SAVE <base64>                    sent right before START when the host shared a save
    START <token> <slot> <players> <input delay> <udp port> <lang> <conf> <picks> <rate>
                                     conf = mode,map,time,score,ff; picks = char:costume:team:input;... by slot
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
import socket
import threading
import time

PROTO = 3
# No I/O/0/1 (they look alike). (The game turns some controller buttons into letter keys; the lobby tells
# those apart from typed letters, so codes can use the whole alphabet.)
CODE_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
NAME_RE = re.compile(r'^[A-Za-z0-9_\-]{1,16}$')
TOKEN_RE = re.compile(r'^[A-Za-z0-9_\-]{1,24}$')   # characters, modes, maps, numbers
OWN_RE = re.compile(r'^[A-Za-z0-9_\-:;.]{1,800}$')  # Peter:0.1.3;Lois:0.2
MAX_LINE = 2 * 1024 * 1024        # a save file (~450 KB) as base64 fits easily
MAX_SAVE = 1024 * 1024
MAX_CLIENTS_PER_IP = 8
MAX_LOBBIES = 500
SESSION_IDLE_S = 120              # forget a game session after this long without packets
LOBBY_KEEP_S = 1800               # a player's place in a lobby waits this long for them to come back
CLIENT_IDLE_S = 60                # drop a lobby connection silent this long (the game pings every 2 s)
SLOT_MOVE_S = 5                   # a slot's address may change (new NAT port) only after this much silence
RELAY_MAX_PPS = 2000              # per player address
RATES = (60, 120, 144)            # online frame rates (everyone in a match runs the same one)


def snap_rate(hz):
    """The highest online frame rate not above hz (60 at least)."""
    return max([r for r in RATES if r <= hz] or [60])

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
        self.char, self.costume, self.team, self.input = 'Peter', '0', '0', 'c'
        self.rate = 60        # the most frames per second their monitor shows
        self.away = False     # in a match (the lobby keeps the place for REJOIN)
        self.gone_at = None   # when an away player's lobby connection closed
        self.match_slot = -1  # their slot in that match

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
        self.conf = ['DM', 'FR', '0', '0', '0', '0']  # mode, map, time, score, friendly fire, rate (0 auto)
        self.own = None                           # the host's characters and costumes
        self.token = None                         # the running match's session token (hex)
        self.playing = False                      # its match runs and the host isn't back yet

    @property
    def host(self):
        return self.members[0]

    def broadcast_members(self):
        entries = ' '.join(f'{i},{c.name},{2 if c.away else int(c.ready)},{c.rtt},{c.char},{c.costume},{c.team},{c.input},'
                           f'{c.rate}' for i, c in enumerate(self.members))
        for c in self.members:
            if not c.away:
                c.send(f'MEMBERS {c.slot} {entries}'.rstrip())

    def broadcast_conf(self, only=None):
        for c in ([only] if only else self.members):
            c.send('CONF ' + ' '.join(self.conf))
            if self.own:
                c.send('OWN ' + self.own)


class Session:
    def __init__(self, players):
        self.addrs = [None] * players
        self.heard = [0.0] * players   # when each slot's address last sent
        self.last = time.monotonic()


class Server:
    def __init__(self, port):
        self.port = port
        self.clients = set()
        self.lobbies = {}
        self.sessions = {}
        self.sock = None
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
                line = await asyncio.wait_for(reader.readline(), CLIENT_IDLE_S)
                if not line:
                    break
                try:
                    self.command(c, line.decode('utf-8', 'replace').strip())
                except Exception as e:  # a bad line must never take the server down
                    log.exception('command failed: %s', e)
                    c.send('ERROR bad request')
                await writer.drain()
        except (asyncio.LimitOverrunError, ValueError, ConnectionError, asyncio.IncompleteReadError,
                asyncio.TimeoutError):
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
                     if l.host.gamekey == c.gamekey and len(l.members) < l.max and not l.private and not l.playing]
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
            elif lobby.playing:
                c.send('ERROR That lobby is in a match - try again when it ends')
            elif lobby.host.gamekey != c.gamekey:
                c.send('ERROR That lobby runs a different version of the game or mod')
            elif len(lobby.members) >= lobby.max:
                c.send('ERROR That lobby is full')
            else:
                self.leave(c)
                taken = {m.name for m in lobby.members}
                if c.name in taken:  # two players with one name: number the newcomer
                    base = c.name[:14]
                    c.name = next(f'{base}{k}' for k in range(2, 10) if f'{base}{k}' not in taken)
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
            if len(args) in (3, 4) and all(TOKEN_RE.match(a) for a in args):
                c.char, c.costume, c.team = args[:3]
                c.input = 'k' if len(args) == 4 and args[3] == 'k' else 'c'
                c.lobby.broadcast_members()
        elif cmd == 'CONF' and c.lobby and c is c.lobby.host:
            if len(args) in (5, 6) and all(TOKEN_RE.match(a) for a in args):
                c.lobby.conf = list(args) + (['0'] if len(args) == 5 else [])
                c.lobby.broadcast_conf()
        elif cmd == 'OWN' and c.lobby and c is c.lobby.host:
            if len(args) == 1 and OWN_RE.match(args[0]):
                c.lobby.own = args[0]
                c.lobby.broadcast_conf()
        elif cmd == 'RATE':
            c.rate = snap_rate(int(args[0])) if args and args[0].isdigit() else 60
            if c.lobby:
                c.lobby.broadcast_members()
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
        elif cmd == 'REJOIN':
            self.rejoin(c, args)
        elif cmd == 'LEAVE':
            self.leave(c)
        else:
            c.send('ERROR unknown command')

    def new_code(self):
        while True:
            code = ''.join(secrets.choice(CODE_LETTERS) for _ in range(4))
            if code not in self.lobbies:
                return code

    def rejoin(self, c, args):
        lobby = self.lobbies.get(args[0].upper()) if args else None
        slot = int(args[2]) if len(args) > 2 and args[2].isdigit() else -1
        old = next((m for m in lobby.members if m.away and m.match_slot == slot), None) if lobby else None
        if not lobby or not lobby.token or len(args) < 2 or args[1] != lobby.token or not old:
            c.send('ERROR That lobby is gone')
            return
        self.leave(c)
        slot = lobby.members.index(old)
        c.name, c.char, c.costume, c.team, c.input = old.name, old.char, old.costume, old.team, old.input
        c.lobby, c.ready, c.away = lobby, False, False
        lobby.members[slot] = c
        if slot == 0:
            lobby.playing = False   # the host is back: the lobby is the lobby again
        c.send(f'LOBBY {lobby.code} {int(lobby.private)}')
        lobby.broadcast_members()
        lobby.broadcast_conf(only=c)
        log.info('%s is back in lobby %s', c.name, lobby.code)

    def leave(self, c):
        lobby = c.lobby
        if not lobby:
            return
        if c.away:   # in its match: the lobby keeps the place (REJOIN)
            c.gone_at = c.gone_at or time.monotonic()
            return
        c.lobby = None
        if lobby.host is c and not any(m.away for m in lobby.members[1:]):  # the host leaving closes the lobby
            self.lobbies.pop(lobby.code, None)
            for m in lobby.members[1:]:
                m.lobby = None
                m.send('CLOSED The host left the lobby')
            log.info('lobby %s closed', lobby.code)
        elif lobby.host is c:   # players still in the match: one of them hosts when they're back
            lobby.members.remove(c)
            lobby.broadcast_members()
        else:
            lobby.members.remove(c)
            lobby.broadcast_members()

    def start(self, lobby):
        for m in [m for m in lobby.members if m.away]:   # still in the last match: they miss this one
            lobby.members.remove(m)
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
        chosen = int(lobby.conf[5]) if len(lobby.conf) > 5 and lobby.conf[5].isdigit() else 0
        rate = snap_rate(chosen) if chosen else min(m.rate for m in lobby.members)   # auto: everyone's monitor
        tick_ms = 1000 / rate
        delay = max(2, min(40, 1 + math.ceil((worst + 15) / tick_ms)))
        conf = ','.join(lobby.conf)
        picks = ';'.join(f'{m.char}:{m.costume}:{m.team}:{m.input}' for m in lobby.members)
        for i, m in enumerate(lobby.members):
            if lobby.save and i > 0:
                m.send('SAVE ' + lobby.save)
            m.send(f'START {token.hex()} {i} {n} {delay} {self.port} {lobby.lang} {conf} {picks} {rate}')
            m.away, m.gone_at, m.ready, m.match_slot = True, None, False, i
        lobby.token, lobby.playing = token.hex(), True   # the lobby waits for its players to come back
        log.info('lobby %s started: %d players, %d fps, input delay %d (worst rtt %d ms)', lobby.code, n, rate, delay, worst)

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
        if s.addrs[slot] != addr:
            # a slot keeps its address; another one may take it over only once it has gone quiet (a player's
            # router can give them a new port), so nobody can grab someone else's slot mid-game
            if s.addrs[slot] is not None and now - s.heard[slot] < SLOT_MOVE_S:
                return
            s.addrs[slot] = addr
        s.heard[slot] = now
        s.last = now
        for i, a in enumerate(s.addrs):
            if a and i != slot:
                try:
                    self.sock.sendto(data, a)
                except OSError:
                    pass

    async def cleanup(self):
        while True:
            await asyncio.sleep(30)
            now = time.monotonic()
            for t in [t for t, s in list(self.sessions.items()) if now - s.last > SESSION_IDLE_S]:
                del self.sessions[t]
            self.rate = {a: r for a, r in list(self.rate.items()) if now - r[0] < 5}
            # lobbies whose players never came back from their match; places nobody reclaimed
            for code, l in list(self.lobbies.items()):
                gone = [m for m in l.members if m.away and m.gone_at and now - m.gone_at > LOBBY_KEEP_S]
                for m in gone:
                    l.members.remove(m)
                if not l.members or all(m.away and m.gone_at for m in l.members) and \
                        min(now - m.gone_at for m in l.members) > LOBBY_KEEP_S:
                    self.lobbies.pop(code, None)
                    log.info('lobby %s closed (nobody came back)', code)
                elif gone:
                    if l.playing and not l.members[0].away:
                        l.playing = False
                    l.broadcast_members()


def relay_thread(server, host, port):
    """The UDP relay: a plain thread with a blocking socket (asyncio's UDP on Windows can stop reading for
    good after a "port unreachable" error - a player's game closing - which ended every later game)."""
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            if hasattr(socket, 'SIO_UDP_CONNRESET'):
                try:
                    sock.ioctl(socket.SIO_UDP_CONNRESET, False)  # Windows: don't report it at all
                except OSError:
                    pass
            sock.bind((host, port))
        except OSError as e:
            log.info('relay: cannot open UDP port %d (%s) - retrying', port, e)
            time.sleep(2)
            continue
        server.sock = sock
        log.info('relay: listening on UDP port %d', port)
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except (ConnectionResetError, ConnectionRefusedError):
                continue  # someone's game closed: nothing to do
            except OSError as e:
                log.info('relay: socket error %s - reopening it', e)
                break
            try:
                server.datagram(data, addr)
            except Exception as e:  # one bad packet must never stop the relay
                log.exception('relay packet failed: %s', e)
        try:
            sock.close()
        except OSError:
            pass
        time.sleep(0.2)


async def main():
    ap = argparse.ArgumentParser(description='FGUY Mod lobby + relay server')
    ap.add_argument('--port', type=int, default=int(os.environ.get('FGUY_PORT', 7650)))
    ap.add_argument('--host', default='0.0.0.0')
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    server = Server(a.port)
    tcp = await asyncio.start_server(server.handle, a.host, a.port, limit=MAX_LINE)
    asyncio.ensure_future(server.cleanup())
    threading.Thread(target=relay_thread, args=(server, a.host, a.port), daemon=True).start()
    log.info('FGUY lobby server on port %d (TCP lobby + UDP relay)', a.port)
    async with tcp:
        await tcp.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
