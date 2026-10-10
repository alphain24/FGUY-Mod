#!/bin/bash
# Installs the FGUY Mod lobby server on Ubuntu/Debian as a service that starts on boot.
# Usage (on the server, in the folder with fguy_lobby.py):   sudo bash install.sh [port]
set -e
PORT="${1:-7650}"
if [ "$(id -u)" != "0" ]; then echo "Run it with sudo: sudo bash install.sh"; exit 1; fi
command -v python3 >/dev/null || { apt-get update && apt-get install -y python3; }

mkdir -p /opt/fguy
install -m 644 "$(dirname "$0")/fguy_lobby.py" /opt/fguy/fguy_lobby.py
id fguy >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin fguy

cat > /etc/systemd/system/fguy-lobby.service <<EOF
[Unit]
Description=FGUY Mod lobby + relay server
After=network-online.target
Wants=network-online.target

[Service]
User=fguy
ExecStart=/usr/bin/python3 /opt/fguy/fguy_lobby.py --port $PORT
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable fguy-lobby >/dev/null
systemctl restart fguy-lobby

# Open the port if the ufw firewall is in use.
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow "$PORT"/tcp >/dev/null
    ufw allow "$PORT"/udp >/dev/null
    echo "Opened port $PORT (TCP + UDP) in ufw."
fi

sleep 1
systemctl --no-pager --lines=3 status fguy-lobby || true
echo
echo "Done. The lobby server runs on port $PORT (TCP + UDP) and starts by itself after a reboot."
echo "If your VPS provider has its own firewall in their website's control panel, allow port $PORT"
echo "for both TCP and UDP there too."
echo "Players set this in FGUYMod\\FGUYMod.ini:   [Online]  Server=$(hostname -I | awk '{print $1}'):$PORT"
