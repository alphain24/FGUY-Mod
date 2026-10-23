// Skin icons in the menus: a skin pack's pictures (icon.png: its tile on the lobby's character screen, head.png: the
// HUD's head - the Skin Studio renders them) shown by the game's own Scaleform menus. A menu script loads
// "fguyicon_<key>.gfx" (MovieClip.loadMovie); ui.cpp's FileOpener hook hands it the movie made here. The game's
// Scaleform takes only movies with its exporter's tag (GFX_ExporterInfo) and draws no bitmaps of a movie's own (its
// pictures come from the game's textures - a DefineBitsLossless shows nothing), so the picture is drawn as one shape:
// its pixels in flat colors (each channel to steps of kStep), every border between two colors one edge with a color
// on each side - no seams, as Flash's own tools save artwork. A pixel is 20 twips: the clip is as many units across as
// the picture is pixels.
#include "common.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_lock;
std::map<std::string, std::vector<unsigned char>> g_png;     // (key: the PNG's)
std::map<std::string, std::vector<unsigned char>> g_movie;   // (made when first asked for - kept: a menu reads from it)
struct Pixels { std::vector<unsigned char> rgba; unsigned w = 0, h = 0; };
std::map<std::string, Pixels> g_pixels;   // (pictures made here - a skin's from the game's own: cosmetics.cpp)

struct Bits {
    std::vector<unsigned char> b;
    unsigned acc = 0;
    int n = 0;
    void Put(unsigned v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc = acc << 1 | ((v >> i) & 1);
            if (++n == 8) { b.push_back((unsigned char)acc); acc = 0; n = 0; }
        }
    }
    void Flush() { if (n) { b.push_back((unsigned char)(acc << (8 - n))); acc = 0; n = 0; } }
};

void Rect(Bits& o, int w, int h) {   // (0..w, 0..h twips)
    o.Put(16, 5);
    o.Put(0, 16); o.Put((unsigned)w, 16); o.Put(0, 16); o.Put((unsigned)h, 16);
    o.Flush();
}

void U16(std::vector<unsigned char>& o, unsigned v) { o.push_back(v & 0xff); o.push_back((v >> 8) & 0xff); }
void U32(std::vector<unsigned char>& o, unsigned v) { U16(o, v & 0xffff); U16(o, v >> 16); }

void Tag(std::vector<unsigned char>& o, unsigned code, const std::vector<unsigned char>& body) {   // (the long form)
    U16(o, code << 6 | 0x3f);
    U32(o, (unsigned)body.size());
    o.insert(o.end(), body.begin(), body.end());
}

const int kStep = 12;   // (a color's steps: fewer colors, longer runs)

struct Edge { int x0, y0, x1, y1; unsigned left, right; };   // (left / right: as it runs - SWF fill styles 0 / 1)

std::vector<unsigned char> Movie(const std::vector<unsigned char>& rgba, unsigned w, unsigned h) {
    // its colors: each pixel's (0: none - mostly transparent)
    std::map<unsigned, unsigned> index;
    std::vector<unsigned> colors, at((size_t)w * h, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const unsigned char* px = &rgba[i * 4];
        if (px[3] < 128) continue;
        unsigned c = 0;
        for (int k = 0; k < 3; ++k) c = c << 8 | (unsigned)((px[k] / kStep * kStep + kStep / 2) > 255 ? 255 : px[k] / kStep * kStep + kStep / 2);
        auto f = index.find(c);
        if (f == index.end()) { colors.push_back(c); f = index.emplace(c, (unsigned)colors.size()).first; }
        at[i] = f->second;
    }
    auto L = [&](int x, int y) -> unsigned { return x < 0 || y < 0 || x >= (int)w || y >= (int)h ? 0 : at[(size_t)y * w + x]; };
    // the borders: down each column line, along each row line - runs of the same two colors as one edge
    std::vector<Edge> edges;
    for (int x = 0; x <= (int)w; ++x)
        for (int y = 0; y < (int)h; ) {
            unsigned a = L(x - 1, y), b = L(x, y);
            if (a == b) { ++y; continue; }
            int y1 = y + 1;
            while (y1 < (int)h && L(x - 1, y1) == a && L(x, y1) == b) ++y1;
            edges.push_back({x, y, x, y1, b, a});   // (going down: the right-hand column on its left)
            y = y1;
        }
    for (int y = 0; y <= (int)h; ++y)
        for (int x = 0; x < (int)w; ) {
            unsigned a = L(x, y - 1), b = L(x, y);
            if (a == b) { ++x; continue; }
            int x1 = x + 1;
            while (x1 < (int)w && L(x1, y - 1) == a && L(x1, y) == b) ++x1;
            edges.push_back({x, y, x1, y, a, b});   // (going right: the row above on its left)
            x = x1;
        }
    std::vector<unsigned char> body, tags;
    body = {0x6f, 0x03, 0, 0, 0, 0, 0x0d, 0, 0, 8, 'f', 'g', 'u', 'y', 'i', 'c', 'o', 'n', 0, 0};   // GFX_ExporterInfo (as the game's)
    Tag(tags, 1000, body);
    body.clear();
    U32(body, 0);   // FileAttributes: nothing
    Tag(tags, 69, body);
    body.clear();   // DefineShape2: id 1
    U16(body, 1);
    Bits s;
    Rect(s, w * 20, h * 20);
    if (colors.size() < 255) s.b.push_back((unsigned char)colors.size());
    else { s.b.push_back(0xff); U16(s.b, (unsigned)colors.size()); }
    for (unsigned c : colors) { s.b.push_back(0); s.b.push_back(c >> 16 & 0xff); s.b.push_back(c >> 8 & 0xff); s.b.push_back(c & 0xff); }
    s.b.push_back(0);   // (no lines)
    int fb = 1;
    while ((1u << fb) <= colors.size()) ++fb;
    s.Put(fb, 4); s.Put(0, 4);
    for (const Edge& e : edges) {
        s.Put(0, 1); s.Put(0, 1); s.Put(0, 1); s.Put(1, 1); s.Put(1, 1); s.Put(1, 1);   // move there, both fills
        s.Put(16, 5); s.Put((unsigned)(e.x0 * 20) & 0xffff, 16); s.Put((unsigned)(e.y0 * 20) & 0xffff, 16);
        s.Put(e.left, fb); s.Put(e.right, fb);
        bool vertical = e.x0 == e.x1;
        s.Put(1, 1); s.Put(1, 1); s.Put(16 - 2, 4); s.Put(0, 1); s.Put(vertical ? 1 : 0, 1);
        s.Put((unsigned)((vertical ? e.y1 - e.y0 : e.x1 - e.x0) * 20) & 0xffff, 16);
    }
    s.Put(0, 6);
    s.Flush();
    body.insert(body.end(), s.b.begin(), s.b.end());
    Tag(tags, 22, body);
    body.clear();   // PlaceObject2: it, at depth 1
    body.push_back(0x02);
    U16(body, 1);
    U16(body, 1);
    Tag(tags, 26, body);
    Tag(tags, 1, {});
    U16(tags, 0);
    std::vector<unsigned char> m = {'G', 'F', 'X', 9, 0, 0, 0, 0};   // (Scaleform's own signature: the game's movies')
    Bits hd;
    Rect(hd, w * 20, h * 20);
    m.insert(m.end(), hd.b.begin(), hd.b.end());
    U16(m, 24 << 8);
    U16(m, 1);
    m.insert(m.end(), tags.begin(), tags.end());
    unsigned len = (unsigned)m.size();
    memcpy(&m[4], &len, 4);
    return m;
}

}  // namespace

std::string IconsAdd(const std::vector<unsigned char>& png) {
    if (png.size() < 8 || memcmp(png.data() + 1, "PNG", 3) != 0) return "";
    unsigned hash = 2166136261u;   // (FNV-1a: the same picture, the same name)
    for (unsigned char c : png) hash = (hash ^ c) * 16777619u;
    char key[16];
    sprintf_s(key, "%08x", hash);
    std::lock_guard<std::mutex> l(g_lock);
    g_png.emplace(key, png);
    return std::string("fguyicon_") + key + ".gfx";
}

void IconsAddPixels(const std::string& key, const std::vector<unsigned char>& rgba, unsigned w, unsigned h) {
    std::lock_guard<std::mutex> l(g_lock);
    g_pixels[key] = {rgba, w, h};
}

bool IconsReady(const std::string& url) {
    size_t a = url.find("fguyicon_"), b = url.find(".gfx");
    if (a == std::string::npos || b == std::string::npos) return false;
    std::string key = url.substr(a + 9, b - a - 9);
    std::lock_guard<std::mutex> l(g_lock);
    return g_png.count(key) || g_pixels.count(key) || g_movie.count(key);
}

const std::vector<unsigned char>* IconsMovie(const char* name) {
    const char* k = name ? strstr(name, "fguyicon_") : nullptr;
    if (!k) return nullptr;
    const char* dot = strstr(k, ".gfx");
    std::string key(k + 9, dot ? (size_t)(dot - k - 9) : strnlen(k + 9, 16));
    std::lock_guard<std::mutex> l(g_lock);
    auto made = g_movie.find(key);
    if (made != g_movie.end()) return &made->second;
    std::vector<unsigned char> rgba;
    unsigned w = 0, h = 0;
    auto px = g_pixels.find(key);
    if (px != g_pixels.end()) { rgba = px->second.rgba; w = px->second.w; h = px->second.h; }
    else {
        auto png = g_png.find(key);
        if (png == g_png.end()) return nullptr;
        if (!SkinDecodePng(png->second, rgba, &w, &h)) return nullptr;
    }
    if (!w || !h || w > 1600 || h > 1600) return nullptr;
    Log(L"Icons: %S - a %ux%u picture", key.c_str(), w, h);
    std::vector<unsigned char>& m = g_movie[key] = Movie(rgba, w, h);
    wchar_t dump[8];   // (test knob FGUYMOD_TEST_ICONDUMP=1: the movie written to FGUYMod\icon_<key>.swf too)
    if (GetEnvironmentVariableW(L"FGUYMOD_TEST_ICONDUMP", dump, 8) && dump[0] == L'1') {
        std::wstring path = std::wstring(g_modDir) + L"\\icon_" + std::wstring(key.begin(), key.end()) + L".swf";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, m.data(), (DWORD)m.size(), &put, nullptr); CloseHandle(f); }
    }
    return &m;
}
