"""Research: each player's match stats in running games, as the scoreboard shows them (the game's
UpdateMultiPlayerStats: score +0xe84, bonus +0xe88, kills +0xe20 + +0xe30, deaths +0xe2c, team, name +0xe90).

  scores.py <pid> [<pid> ...]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memwatch import players, reader  # noqa: E402


def stats(r, p):
    i = lambda o: struct.unpack('<i', r(p + o, 4))[0]
    name = (r(p + 0xe90, 32) or b'').split(b'\0')[0].decode('latin1', 'replace')
    return {'player': i(0xdf8), 'name': name, 'score': i(0xe84), 'bonus': i(0xe88), 'kills': i(0xe20) + i(0xe30),
            'kills_a': i(0xe20), 'kills_b': i(0xe30), 'deaths': i(0xe2c)}


def main():
    for pid in map(int, sys.argv[1:]):
        r = reader(pid)
        for p in players(r):
            s = stats(r, p)
            if s['player'] < 0 or s['player'] > 3:
                continue
            print('%6d  player %d %-10s score %4d bonus %4d kills %3d (%d+%d) deaths %3d' % (
                pid, s['player'], s['name'], s['score'], s['bonus'], s['kills'], s['kills_a'], s['kills_b'], s['deaths']))


if __name__ == '__main__':
    main()
