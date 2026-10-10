"""Self-test for fguy_lobby.py: starts a server on a spare port and plays a whole lobby through it.

Usage: python test_lobby.py
"""
import base64
import os
import socket
import subprocess
import sys
import time

PORT = 7659
HERE = os.path.dirname(os.path.abspath(__file__))


class Player:
    def __init__(self, name):
        self.s = socket.create_connection(('127.0.0.1', PORT))
        self.f = self.s.makefile('rw', encoding='utf-8', newline='\n')
        self.send(f'HELLO 2 testgame {name}')
        assert self.expect('WELCOME')

    def send(self, line):
        self.f.write(line + '\n')
        self.f.flush()

    def expect(self, word):
        while True:
            line = self.f.readline().strip()
            print('   <-', line[:100])
            if line.startswith(word):
                return line
            assert not line.startswith('ERROR'), line


def main():
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'fguy_lobby.py'), '--port', str(PORT)])
    try:
        time.sleep(1)
        host, guest = Player('Host'), Player('Guest')
        host.send('HOST 4')
        code = host.expect('LOBBY').split()[1]
        guest.send('LIST')
        assert code in guest.expect('LIST')
        guest.send('JOIN ' + code)
        guest.expect('LOBBY')
        assert guest.expect('MEMBERS').startswith('MEMBERS 1 ')
        host.send('START')  # guest not ready yet: refused
        line = host.f.readline().strip()
        while not line.startswith('ERROR'):
            line = host.f.readline().strip()
        assert 'ready' in line, line
        guest.send('SET Lois 2 1')
        assert ',Lois,2,1' in host.expect('MEMBERS')
        host.send('CONF CF AM 1 2 1')
        while guest.expect('CONF') != 'CONF CF AM 1 2 1':  # the first one is the defaults, sent on join
            pass
        guest.send('RTT 80')
        guest.send('READY 1')
        host.send('SAVE ' + base64.b64encode(b'save bytes').decode())
        host.send('START')
        start_host = host.expect('START').split()
        assert base64.b64decode(guest.expect('SAVE').split()[1]) == b'save bytes'
        start_guest = guest.expect('START').split()
        token = bytes.fromhex(start_host[1])
        assert start_guest[1] == start_host[1] and start_host[2] == '0' and start_guest[2] == '1'
        assert start_guest[7] == 'CF,AM,1,2,1' and start_guest[8] == 'Peter:0:0;Lois:2:1', start_guest
        print('   input delay for 80 ms:', start_host[4])

        # relay: both players say hello, then host -> guest
        u0, u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u1.settimeout(2)
        u1.sendto(b'FG' + token + b'\x01hello', ('127.0.0.1', PORT))
        time.sleep(0.2)
        u0.sendto(b'FG' + token + b'\x00inputs', ('127.0.0.1', PORT))
        data, _ = u1.recvfrom(1500)
        assert data == b'FG' + token + b'\x00inputs', data
        print('PASS: lobby, save sharing, start and relay all work')
    finally:
        srv.terminate()


if __name__ == '__main__':
    main()
