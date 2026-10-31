// Building a skin's texture on the player's own PC (a skin pack of format 2): a pack carries no game art - only what its
// maker did to the game's texture, the recolors (each: the picked color, the new one, how close; done as the Skin
// Studio's tools/cosmetics/js/app.js recolor does them) and an overlay (a PNG: only the pixels they painted themselves,
// the rest transparent). Here: the game's own texture (as the GPU has it: DXT1 / DXT5 / 32-bit) read back, the
// recolors done on it, the overlay laid on top, and the result written as a DDS of the game texture's own format and
// mip levels (what cosmetics.cpp makes its texture from).
#include "common.h"

#include <d3d9.h>
#include <wincodec.h>

#include <cmath>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

using Rgba = std::vector<unsigned char>;

void From565(unsigned c, double* out) {
    out[0] = ((c >> 11) & 31) * 255.0 / 31;
    out[1] = ((c >> 5) & 63) * 255.0 / 63;
    out[2] = (c & 31) * 255.0 / 31;
}

unsigned char Byte(double v) {   // (as the browser's Uint8ClampedArray stores a number: rounded, kept in 0..255)
    if (!(v > 0)) return 0;
    if (v >= 255) return 255;
    return (unsigned char)nearbyint(v);
}

// one 4x4 color block (the Studio's dds.js colorBlock)
void ColorBlock(const unsigned char* b, Rgba& out, unsigned x0, unsigned y0, unsigned w, unsigned h, bool dxt1) {
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, bits = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24;
    double pal[4][4];
    From565(c0, pal[0]);
    From565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (!dxt1 || c0 > c1) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = !dxt1 || c0 > c1 ? 255 : 0;
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            unsigned px = x0 + x, py = y0 + y;
            if (px >= w || py >= h) continue;
            const double* p = pal[(bits >> (2 * (y * 4 + x))) & 3];
            for (int k = 0; k < 4; ++k) out[(py * w + px) * 4 + k] = Byte(p[k]);
        }
}

// the game's texture, its largest level, as RGBA
bool ReadTexture(IDirect3DTexture9* tex, Rgba& out, unsigned* w, unsigned* h, D3DFORMAT* fmt, unsigned* mips) {
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d))) return false;
    *w = d.Width; *h = d.Height; *fmt = d.Format; *mips = tex->GetLevelCount();
    if (d.Format != D3DFMT_DXT1 && d.Format != D3DFMT_DXT5 && d.Format != D3DFMT_DXT3 && d.Format != D3DFMT_A8R8G8B8) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return false;
    out.assign((size_t)d.Width * d.Height * 4, 0);
    const unsigned char* src = static_cast<const unsigned char*>(lr.pBits);
    if (d.Format == D3DFMT_A8R8G8B8) {
        for (unsigned y = 0; y < d.Height; ++y)
            for (unsigned x = 0; x < d.Width; ++x) {
                const unsigned char* p = src + y * lr.Pitch + x * 4;
                unsigned char* o = &out[(y * d.Width + x) * 4];
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
            }
    } else {
        bool dxt1 = d.Format == D3DFMT_DXT1;
        unsigned bs = dxt1 ? 8 : 16;
        for (unsigned by = 0; by < d.Height; by += 4)
            for (unsigned bx = 0; bx < d.Width; bx += 4) {
                const unsigned char* b = src + (by / 4) * lr.Pitch + (bx / 4) * bs;
                double alpha[16];
                bool hasAlpha = !dxt1;
                if (d.Format == D3DFMT_DXT3) {
                    for (int i = 0; i < 16; ++i) alpha[i] = ((b[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
                } else if (d.Format == D3DFMT_DXT5) {
                    double a0 = b[0], a1 = b[1], pal[8] = {a0, a1};
                    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                    else { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
                    unsigned long long bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= (unsigned long long)b[2 + i] << (8 * i);
                    for (int i = 0; i < 16; ++i) alpha[i] = pal[(bits >> (3 * i)) & 7];
                }
                ColorBlock(b + (dxt1 ? 0 : 8), out, bx, by, d.Width, d.Height, dxt1);
                if (hasAlpha)
                    for (int i = 0; i < 16; ++i) {
                        unsigned px = bx + (i & 3), py = by + (i >> 2);
                        if (px < d.Width && py < d.Height) out[(py * d.Width + px) * 4 + 3] = Byte(alpha[i]);
                    }
            }
    }
    tex->UnlockRect(0);
    return true;
}

// ---- the recolor (app.js recolor: the picked color's pixels to the new one, their own light and shade kept) ----------
void ToHsl(double r, double g, double b, double* hsl) {
    r /= 255; g /= 255; b /= 255;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), l = (mx + mn) / 2;
    if (mx == mn) { hsl[0] = 0; hsl[1] = 0; hsl[2] = l; return; }
    double d = mx - mn, s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    double h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    hsl[0] = h / 6; hsl[1] = s; hsl[2] = l;
}
double Hue(double p, double q, double t) {
    t = fmod(t + 1, 1);
    return t < 1.0 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t) * 6 : p;
}
void FromHsl(double h, double s, double l, double* rgb) {
    if (!s) { rgb[0] = rgb[1] = rgb[2] = l * 255; return; }
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    rgb[0] = Hue(p, q, h + 1.0 / 3) * 255;
    rgb[1] = Hue(p, q, h) * 255;
    rgb[2] = Hue(p, q, h - 1.0 / 3) * 255;
}

void Recolor(Rgba& d, const double* P, const double* Q, double tol) {
    double hp[3], hq[3];
    ToHsl(P[0], P[1], P[2], hp);
    ToHsl(Q[0], Q[1], Q[2], hq);
    for (size_t i = 0; i + 3 < d.size(); i += 4) {
        double dist = sqrt((d[i] - P[0]) * (d[i] - P[0]) + (d[i + 1] - P[1]) * (d[i + 1] - P[1]) + (d[i + 2] - P[2]) * (d[i + 2] - P[2]));
        if (dist > tol) continue;
        double w = dist < tol * 0.6 ? 1 : 1 - (dist - tol * 0.6) / (tol * 0.4);
        double hc[3], out[3];
        ToHsl(d[i], d[i + 1], d[i + 2], hc);
        // (its saturation shifted by as much as the two colors' differ, like its lightness - not scaled: a near-white picked
        //  color made every slightly tinted pixel near it fully saturated)
        double s = fmax(0, fmin(1, hc[1] + hq[1] - hp[1])), l = fmax(0, fmin(1, hc[2] + hq[2] - hp[2]));
        FromHsl(hq[0], s, l, out);
        for (int k = 0; k < 3; ++k) d[i + k] = Byte(d[i + k] * (1 - w) + out[k] * w);
    }
}

// ---- the overlay (a PNG, through Windows' own image decoder) --------------------------------------------------------
bool DecodePng(const std::vector<unsigned char>& png, unsigned w, unsigned h, Rgba& out, unsigned* gotW = nullptr,
               unsigned* gotH = nullptr) {   // (w, h 0: any size - gotW, gotH)
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), (DWORD)png.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&fw, &fh)) && (!w || (fw == w && fh == h)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        out.assign((size_t)fw * fh * 4, 0);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)out.size(), out.data()));
        if (gotW) *gotW = fw;
        if (gotH) *gotH = fh;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// ---- writing it as the game's format ---------------------------------------------------------------------------------
unsigned To565(const double* c) {
    auto q = [](double v, int top) { v = v < 0 ? 0 : v > 255 ? 255 : v; return (unsigned)nearbyint(v * top / 255); };
    return q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31);
}

// a block's 16 pixels coded with two colors (c0 > c1: four colors, never the transparent mode) - how far off it is
double CodeBlock(const unsigned char px[16][4], unsigned c0, unsigned c1, unsigned* bits) {
    if (c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) { if (c1 > 0) --c1; else ++c0; }
    double pal[4][3];
    From565(c0, pal[0]);
    From565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    double err = 0;
    *bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        double bd = 1e30;
        for (int j = 0; j < 4; ++j) {
            double d = 0;
            for (int k = 0; k < 3; ++k) d += (px[i][k] - pal[j][k]) * (px[i][k] - pal[j][k]);
            if (d < bd) { bd = d; best = j; }
        }
        *bits |= (unsigned)best << (2 * i);
        err += bd;
    }
    bits[1] = c0 | c1 << 16;
    return err;
}

// One 4x4 block as DXT1. Its two colors: the ends of the line its pixels lie along most (their principal axis - the box
// of their red, green and blue ranges alone puts colors on edges that aren't in the picture: speckled outlines), then
// moved to fit the pixels best for the colors they were given (least squares, twice); whichever is closest.
void EncodeColorBlock(const unsigned char px[16][4], unsigned char* o) {
    double mean[3] = {0, 0, 0}, cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k) mean[k] += px[i][k] / 16.0;
    for (int i = 0; i < 16; ++i) {
        double r = px[i][0] - mean[0], g = px[i][1] - mean[1], b = px[i][2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    double axis[3] = {0.577, 0.577, 0.577};
    for (int it = 0; it < 8; ++it) {   // (power iteration)
        double v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                       cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                       cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-9) break;
        for (int k = 0; k < 3; ++k) axis[k] = v[k] / n;
    }
    double tmin = 1e30, tmax = -1e30;
    for (int i = 0; i < 16; ++i) {
        double t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        tmin = fmin(tmin, t); tmax = fmax(tmax, t);
    }
    double e0[3], e1[3];
    for (int k = 0; k < 3; ++k) { e0[k] = mean[k] + axis[k] * tmax; e1[k] = mean[k] + axis[k] * tmin; }
    unsigned bits[2], best[2];
    double bestErr = CodeBlock(px, To565(e0), To565(e1), best);
    for (int it = 0; it < 2; ++it) {   // (the colors that fit the pixels best, each given the color it was coded with)
        static const double w0[4] = {1, 0, 2.0 / 3, 1.0 / 3};
        double aa = 0, ab = 0, bb = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            double a = w0[(best[0] >> (2 * i)) & 3], b = 1 - a;
            aa += a * a; ab += a * b; bb += b * b;
            for (int k = 0; k < 3; ++k) { ax[k] += a * px[i][k]; bx[k] += b * px[i][k]; }
        }
        double det = aa * bb - ab * ab;
        if (fabs(det) < 1e-9) break;
        for (int k = 0; k < 3; ++k) { e0[k] = (ax[k] * bb - bx[k] * ab) / det; e1[k] = (bx[k] * aa - ax[k] * ab) / det; }
        double err = CodeBlock(px, To565(e0), To565(e1), bits);
        if (err >= bestErr) break;
        bestErr = err; best[0] = bits[0]; best[1] = bits[1];
    }
    double lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};   // (the box's corners - a flat or two-tone block can be closer)
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k) { lo[k] = fmin(lo[k], px[i][k]); hi[k] = fmax(hi[k], px[i][k]); }
    if (CodeBlock(px, To565(hi), To565(lo), bits) < bestErr) { best[0] = bits[0]; best[1] = bits[1]; }
    unsigned c0 = best[1] & 0xffff, c1 = best[1] >> 16;
    o[0] = c0 & 255; o[1] = (unsigned char)(c0 >> 8); o[2] = c1 & 255; o[3] = (unsigned char)(c1 >> 8);
    o[4] = best[0] & 255; o[5] = (best[0] >> 8) & 255; o[6] = (best[0] >> 16) & 255; o[7] = (unsigned char)(best[0] >> 24);
}

void EncodeAlphaBlock(const unsigned char px[16][4], unsigned char* o) {
    int a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) { a0 = a0 > px[i][3] ? a0 : px[i][3]; a1 = a1 < px[i][3] ? a1 : px[i][3]; }
    if (a0 == a1) { a1 = a0 > 0 ? a0 - 1 : 0; if (a0 == 0) a0 = 1; }
    double pal[8] = {(double)a0, (double)a1};
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7.0;
    unsigned long long bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        double bd = 1e30;
        for (int j = 0; j < 8; ++j) if (fabs(px[i][3] - pal[j]) < bd) { bd = fabs(px[i][3] - pal[j]); best = j; }
        bits |= (unsigned long long)best << (3 * i);
    }
    o[0] = (unsigned char)a0; o[1] = (unsigned char)a1;
    for (int i = 0; i < 6; ++i) o[2 + i] = (unsigned char)(bits >> (8 * i));
}

Rgba Halve(const Rgba& d, unsigned w, unsigned h, unsigned* nw, unsigned* nh) {
    *nw = w > 1 ? w >> 1 : 1; *nh = h > 1 ? h >> 1 : 1;
    Rgba out((size_t)*nw * *nh * 4);
    for (unsigned y = 0; y < *nh; ++y)
        for (unsigned x = 0; x < *nw; ++x)
            for (int k = 0; k < 4; ++k) {
                unsigned s = 0;
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        unsigned sx = x * 2 + dx < w ? x * 2 + dx : w - 1, sy = y * 2 + dy < h ? y * 2 + dy : h - 1;
                        s += d[(sy * w + sx) * 4 + k];
                    }
                out[(y * *nw + x) * 4 + k] = (unsigned char)(s / 4);
            }
    return out;
}

// twice the size, smooth (each new pixel from the four nearest: as the Studio's canvas draws it bigger)
Rgba Double(const Rgba& d, unsigned w, unsigned h) {
    Rgba out((size_t)w * h * 16);
    for (unsigned y = 0; y < h * 2; ++y)
        for (unsigned x = 0; x < w * 2; ++x) {
            double sx = (x + 0.5) / 2 - 0.5, sy = (y + 0.5) / 2 - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double fx = sx - x0, fy = sy - y0;
            auto at = [&](int xx, int yy, int k) {
                xx = xx < 0 ? 0 : xx >= (int)w ? w - 1 : xx;
                yy = yy < 0 ? 0 : yy >= (int)h ? h - 1 : yy;
                return (double)d[((size_t)yy * w + xx) * 4 + k];
            };
            for (int k = 0; k < 4; ++k)
                out[((size_t)y * w * 2 + x) * 4 + k] = Byte((at(x0, y0, k) * (1 - fx) + at(x0 + 1, y0, k) * fx) * (1 - fy) +
                                                            (at(x0, y0 + 1, k) * (1 - fx) + at(x0 + 1, y0 + 1, k) * fx) * fy);
        }
    return out;
}

// The overlay on the pixels (w x h). The Studio keeps a texture at twice or four times the game's size (up to 2048) when
// a picture that big is put on it - the skin's texture is made that big then (sharper than the game's own); for a
// picture of the game's size it's laid on it made small again. Any other size: left out.
bool LayOverlay(Rgba& px, unsigned* w, unsigned* h, unsigned* mips, const std::vector<unsigned char>& overlay, bool grow) {
    Rgba ov;
    unsigned ow = 0, oh = 0;
    if (!DecodePng(overlay, 0, 0, ov, &ow, &oh)) { Log(L"Cosmetics: a skin's overlay isn't a PNG - left out"); return false; }
    for (int i = 0; i < 2 && ow > *w && oh > *h && ow % *w == 0 && oh % *h == 0 && ow / *w == oh / *h && ow <= 2048 && oh <= 2048; ++i) {
        if (grow) {
            px = Double(px, *w, *h);
            *w *= 2; *h *= 2;
            if (mips && *mips > 1) ++*mips;   // (down to the same smallest level)
        } else {
            unsigned nw, nh;
            ov = Halve(ov, ow, oh, &nw, &nh);
            ow = nw; oh = nh;
        }
    }
    if (ow != *w || oh != *h) {
        Log(L"Cosmetics: a skin's overlay is %ux%u, its texture %ux%u - left out", ow, oh, *w, *h);
        return false;
    }
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        double a = ov[i + 3] / 255.0;
        if (a <= 0) continue;
        for (int k = 0; k < 3; ++k) px[i + k] = Byte(ov[i + k] * a + px[i + k] * (1 - a));
    }
    return true;
}

void PutU32(std::vector<unsigned char>& b, size_t at, unsigned v) { memcpy(&b[at], &v, 4); }

std::vector<unsigned char> EncodeDds(const Rgba& rgba, unsigned width, unsigned height, D3DFORMAT fmt, unsigned mips) {
    bool dxt = fmt != D3DFMT_A8R8G8B8, dxt1 = fmt == D3DFMT_DXT1;
    std::vector<unsigned char> out(128, 0), body;
    Rgba cur = rgba;
    unsigned w = width, h = height, levels = 0, first = 0;
    for (unsigned l = 0; l < mips; ++l) {
        size_t start = body.size();
        if (!dxt) {
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                body.push_back(cur[i * 4 + 2]); body.push_back(cur[i * 4 + 1]); body.push_back(cur[i * 4]); body.push_back(cur[i * 4 + 3]);
            }
        } else {
            unsigned bw = (w + 3) / 4 ? (w + 3) / 4 : 1, bh = (h + 3) / 4 ? (h + 3) / 4 : 1, bs = dxt1 ? 8 : 16;
            body.resize(start + (size_t)bw * bh * bs);
            for (unsigned by = 0; by < bh; ++by)
                for (unsigned bx = 0; bx < bw; ++bx) {
                    unsigned char px[16][4];
                    for (int i = 0; i < 16; ++i) {
                        unsigned x = bx * 4 + (i & 3) < w ? bx * 4 + (i & 3) : w - 1, y = by * 4 + (i >> 2) < h ? by * 4 + (i >> 2) : h - 1;
                        memcpy(px[i], &cur[(y * w + x) * 4], 4);
                    }
                    unsigned char* o = &body[start + (by * bw + bx) * bs];
                    if (dxt1) EncodeColorBlock(px, o);
                    else { EncodeAlphaBlock(px, o); EncodeColorBlock(px, o + 8); }
                }
        }
        if (l == 0) first = (unsigned)(body.size() - start);
        ++levels;
        if (w == 1 && h == 1) break;
        unsigned nw, nh;
        cur = Halve(cur, w, h, &nw, &nh);
        w = nw; h = nh;
    }
    PutU32(out, 0, 0x20534444);
    PutU32(out, 4, 124);
    PutU32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (dxt ? 0x80000 : 0x8));
    PutU32(out, 12, height);
    PutU32(out, 16, width);
    PutU32(out, 20, dxt ? first : width * 4);
    PutU32(out, 28, levels);
    PutU32(out, 76, 32);
    if (dxt) {
        PutU32(out, 80, 0x4);
        PutU32(out, 84, dxt1 ? MAKEFOURCC('D', 'X', 'T', '1') : MAKEFOURCC('D', 'X', 'T', '5'));
    } else {
        PutU32(out, 80, 0x41); PutU32(out, 88, 32);
        PutU32(out, 92, 0xff0000); PutU32(out, 96, 0xff00); PutU32(out, 100, 0xff); PutU32(out, 104, 0xff000000);
    }
    PutU32(out, 108, 0x1000 | (levels > 1 ? 0x400008 : 0));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

// cosmetics.cpp: the skin's texture from the game's own one - its recolors ([from r g b, to r g b, how close] each,
// in order) and its overlay (a PNG, or none) - as a DDS of the game texture's format and levels; false: it can't be
bool SkinBuildTexture(IDirect3DTexture9* game, const std::vector<double>& ops, const std::vector<unsigned char>& overlay,
                      std::vector<unsigned char>& dds) {
    Rgba px;
    unsigned w = 0, h = 0, mips = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    if (!game || !ReadTexture(game, px, &w, &h, &fmt, &mips)) return false;
    for (size_t i = 0; i + 7 <= ops.size(); i += 7) Recolor(px, &ops[i], &ops[i + 3], ops[i + 6]);
    if (!overlay.empty()) LayOverlay(px, &w, &h, &mips, overlay, true);
    dds = EncodeDds(px, w, h, fmt == D3DFMT_DXT3 ? D3DFMT_DXT5 : fmt, mips);
    return true;
}

// cosmetics.cpp: a skin's picture made here - the game's texture's pixels (RGBA) with the recolors and the overlay on
bool SkinBuildRgba(IDirect3DTexture9* game, const std::vector<double>& ops, const std::vector<unsigned char>& overlay,
                   std::vector<unsigned char>& rgba, unsigned* w, unsigned* h) {
    unsigned mips = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    if (!game || !ReadTexture(game, rgba, w, h, &fmt, &mips)) return false;
    for (size_t i = 0; i + 7 <= ops.size(); i += 7) Recolor(rgba, &ops[i], &ops[i + 3], ops[i + 6]);
    if (!overlay.empty()) LayOverlay(rgba, w, h, nullptr, overlay, false);   // (a picture: the game's size)
    return true;
}

// icons.cpp: a PNG's pixels (RGBA)
bool SkinDecodePng(const std::vector<unsigned char>& png, std::vector<unsigned char>& rgba, unsigned* w, unsigned* h) {
    return DecodePng(png, 0, 0, rgba, w, h);
}

// models.cpp: a model's texture (a PNG, its own art) as a DXT5 DDS with all its mip levels
bool SkinPngToDds(const std::vector<unsigned char>& png, std::vector<unsigned char>& dds) {
    Rgba px;
    unsigned w = 0, h = 0;
    if (!DecodePng(png, 0, 0, px, &w, &h) || !w || !h) return false;
    unsigned mips = 1;
    while ((w >> mips) || (h >> mips)) ++mips;
    dds = EncodeDds(px, w, h, D3DFMT_DXT5, mips);
    return true;
}
