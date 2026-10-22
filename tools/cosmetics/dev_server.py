"""Development: serves the Skin Studio on http://127.0.0.1:8766 (this PC only), plus /game/<file> from a game folder so
the page can open it without the folder dialog (index.html?dev). Players never use this - they open their own folder.

  python tools/cosmetics/dev_server.py [game folder]   (default: A:/Games/FGUY-Research)"""
import functools
import http.server
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = sys.argv[1] if len(sys.argv) > 1 else 'A:/Games/FGUY-Research'
PLAYERS = os.path.join(GAME, 'FGUY', 'Global_Scenes', 'Players')


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        p = path.split('?')[0]
        if p.startswith('/game/'):
            return os.path.join(PLAYERS, os.path.basename(p))
        return super().translate_path(path)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


if __name__ == '__main__':
    http.server.ThreadingHTTPServer(('127.0.0.1', 8766), functools.partial(Handler, directory=HERE)).serve_forever()
