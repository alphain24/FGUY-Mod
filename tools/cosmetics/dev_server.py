"""Development: serves the Skin Studio on http://127.0.0.1:8766 (this PC only), plus /game/<file> from a game folder so
the page can open it without the folder dialog (index.html?dev). Players never use this - they open their own folder.

  python tools/cosmetics/dev_server.py [game folder]   (default: A:/Games/FGUY-Research)"""
import functools
import http.server
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
GAME = sys.argv[1] if len(sys.argv) > 1 else 'A:/Games/FGUY-Research'
PLAYERS = os.path.join(GAME, 'FGUY', 'Global_Scenes', 'Players')


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        p = path.split('?')[0]
        if p.startswith('/game/'):
            return os.path.join(PLAYERS, os.path.basename(p))
        if p.startswith('/research/'):   # build/research (local exports, never published)
            return os.path.join(REPO, 'build', 'research', os.path.basename(p))
        if p.startswith('/content/'):    # content/<piece>/... (the img2threejs work and its review page)
            rel = os.path.normpath(p[len('/content/'):]).lstrip('\\/')
            if not rel.startswith('..'):
                return os.path.join(REPO, 'content', rel)
        return super().translate_path(path)

    def do_PUT(self):   # (testing: the page saves a pack to build/research - PUT /save/<name>)
        name = os.path.basename(self.path.split('?')[0])
        out = os.path.join(HERE, '..', '..', 'build', 'research')
        os.makedirs(out, exist_ok=True)
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        open(os.path.join(out, name), 'wb').write(data)
        self.send_response(200)
        self.end_headers()

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


if __name__ == '__main__':
    http.server.ThreadingHTTPServer(('127.0.0.1', 8766), functools.partial(Handler, directory=HERE)).serve_forever()
