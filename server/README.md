# FGUY Mod lobby server

`fguy_lobby.py` runs the in-game **Main Menu › ONLINE** lobbies, and relays game traffic between
players during a session, so nobody needs to open ports on their router. It's one Python file with no
extra packages. It uses one port number (default **7650**) for both TCP (lobby) and UDP (relay).

## Install on an Ubuntu VPS

From your PC (PowerShell, in the project folder), copy the two files to the server. Replace `USER` and
`SERVER-IP` with your VPS login and address:

```
scp server\fguy_lobby.py server\install.sh USER@SERVER-IP:~
```

Then log in to the server and run the installer:

```
ssh USER@SERVER-IP
sudo bash install.sh
```

That's it: the server runs as the `fguy-lobby` service and starts by itself after a reboot. If your
VPS provider has a firewall in its web control panel, allow port 7650 for **TCP and UDP** there too.

Useful commands on the server:

| What | Command |
| --- | --- |
| Is it running? | `systemctl status fguy-lobby` |
| Live log | `journalctl -u fguy-lobby -f` |
| Restart | `sudo systemctl restart fguy-lobby` |
| Update | copy the new `fguy_lobby.py` over, then `sudo bash install.sh` again |
| Remove | `sudo systemctl disable --now fguy-lobby` |

## Run it locally (testing)

```
python server\fguy_lobby.py
python server\test_lobby.py        # self-test on a spare port
```

## Point the game at a server

`FGUYMod\FGUYMod.ini`:

```
[Online]
Server=SERVER-IP:7650
```

## How a session works

1. Players open ONLINE. One hosts, the others join from the list, everyone readies up, the host starts.
2. The server sends everyone a session token, their player slot and an input delay. It picks the delay
   from the slowest player's ping, so inputs always arrive in time.
3. Every game restarts into the session. It uses a copy of the host's save (stored in
   `FGUYMod\online\`; nobody's own save is touched). Every copy boots to the main menu in lockstep, and
   each remote player is one more controller. The game's own menus handle the rest: Story co-op
   drop-in, and the Multiplayer modes with their join screens.
4. During play the server only forwards small UDP packets: about 60 per second per player.
