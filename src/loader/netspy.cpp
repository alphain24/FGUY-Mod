// Research logging: the game's own debug prints and its Winsock traffic go to our log.
// The exe imports ws2_32 by ordinal; types are spelled out by hand so we don't need winsock headers.

#include "common.h"

#include <cwchar>

namespace {

decltype(&OutputDebugStringW) Real_OutputDebugStringW;
decltype(&OutputDebugStringA) Real_OutputDebugStringA;

void TrimNewline(wchar_t* s) {
    for (size_t n = wcslen(s); n && (s[n - 1] == L'\n' || s[n - 1] == L'\r'); --n) s[n - 1] = 0;
}

// Some of the game's prints hand plain text to the wide-text call: two letters became one Chinese-looking character
// in the log ("StoreStats - success" showed as CJK). Such a string - every character's two bytes printable
// ASCII (the last one's high byte maybe its end) - is read as the plain text it is.
bool LooksNarrow(LPCWSTR s) {
    int n = 0;
    for (; s[n] && n < 512; ++n) {
        unsigned lo = s[n] & 0xFF, hi = s[n] >> 8;
        bool loOk = (lo >= 0x20 && lo < 0x7F) || lo == '\n' || lo == '\r' || lo == '\t';
        bool hiOk = (hi >= 0x20 && hi < 0x7F) || hi == '\n' || hi == '\r' || hi == '\t' || (hi == 0 && !s[n + 1]);
        if (!loOk || !hiOk) return false;
    }
    return n > 0;
}

void WINAPI Hook_OutputDebugStringW(LPCWSTR s) {
    if (s) {
        wchar_t buf[1024];
        if (LooksNarrow(s)) MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(s), -1, buf, 1024);
        else wcsncpy_s(buf, s, _TRUNCATE);
        buf[1023] = 0;
        TrimNewline(buf);
        Log(L"Game: %s", buf);
    }
    Real_OutputDebugStringW(s);
}

void WINAPI Hook_OutputDebugStringA(LPCSTR s) {
    if (s) {
        wchar_t buf[1024];
        MultiByteToWideChar(CP_ACP, 0, s, -1, buf, 1024);
        buf[1023] = 0;
        TrimNewline(buf);
        Log(L"Game: %s", buf);
    }
    Real_OutputDebugStringA(s);
}

using socket_t = int(WINAPI*)(int, int, int);
using bind_t = int(WINAPI*)(UINT_PTR, const BYTE*, int);
using sendto_t = int(WINAPI*)(UINT_PTR, const char*, int, int, const BYTE*, int);
using send_t = int(WINAPI*)(UINT_PTR, const char*, int, int);
using recvfrom_t = int(WINAPI*)(UINT_PTR, char*, int, int, BYTE*, int*);
using recv_t = int(WINAPI*)(UINT_PTR, char*, int, int);
socket_t Real_socket;
bind_t Real_bind, Real_connect;
sendto_t Real_sendto;
send_t Real_send;
recvfrom_t Real_recvfrom;
recv_t Real_recv;
int g_netLogBudget = 30;   // the game sends a LAN beacon every second; the first few calls say enough
int g_netDumpBudget = 40;  // packets whose bytes we hex-dump

void LogHex(const wchar_t* tag, const char* buf, int len) {
    if (g_netDumpBudget <= 0 || len <= 0) return;
    // Skip exact repeats of the last dumped packet (e.g. LAN beacons sent every second).
    static unsigned long long lastHash = 0;
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < len; ++i) h = (h ^ (unsigned char)buf[i]) * 1099511628211ull;
    if (h == lastHash) return;
    lastHash = h;
    --g_netDumpBudget;
    wchar_t line[3 * 64 + 1];
    int n = len < 64 ? len : 64;
    for (int i = 0; i < n; ++i) swprintf_s(line + 3 * i, 4, L"%02x ", (unsigned char)buf[i]);
    line[3 * n] = 0;
    Log(L"Net:   %s bytes: %s%s", tag, line, len > 64 ? L"..." : L"");
}

void FormatAddr(const BYTE* sa, wchar_t* out, size_t n) {
    if (!sa) { wcscpy_s(out, n, L"-"); return; }
    // sockaddr_in: family(2) port(2, big-endian) addr(4)
    swprintf_s(out, n, L"%u.%u.%u.%u:%u", sa[4], sa[5], sa[6], sa[7], (sa[2] << 8) | sa[3]);
}

int WINAPI Hook_socket(int af, int type, int proto) {
    int s = Real_socket(af, type, proto);
    Log(L"Net: socket(af=%d, type=%s, proto=%d) -> %d", af,
        type == 1 ? L"TCP" : type == 2 ? L"UDP" : L"?", proto, s);
    return s;
}
int WINAPI Hook_bind(UINT_PTR s, const BYTE* sa, int len) {
    wchar_t a[64]; FormatAddr(sa, a, 64);
    int r = Real_bind(s, sa, len);
    Log(L"Net: bind(sock=%u, %s) -> %d", (unsigned)s, a, r);
    return r;
}
int WINAPI Hook_connect(UINT_PTR s, const BYTE* sa, int len) {
    wchar_t a[64]; FormatAddr(sa, a, 64);
    int r = Real_connect(s, sa, len);
    Log(L"Net: connect(sock=%u, %s) -> %d", (unsigned)s, a, r);
    return r;
}
int WINAPI Hook_sendto(UINT_PTR s, const char* buf, int len, int flags, const BYTE* sa, int salen) {
    int r = Real_sendto(s, buf, len, flags, sa, salen);
    if (g_netLogBudget > 0) {
        --g_netLogBudget;
        wchar_t a[64]; FormatAddr(sa, a, 64);
        Log(L"Net: sendto(sock=%u, %d bytes, to %s) -> %d", (unsigned)s, len, a, r);
    }
    LogHex(L"sendto", buf, len);
    return r;
}
int WINAPI Hook_send(UINT_PTR s, const char* buf, int len, int flags) {
    int r = Real_send(s, buf, len, flags);
    if (g_netLogBudget > 0) { --g_netLogBudget; Log(L"Net: send(sock=%u, %d bytes) -> %d", (unsigned)s, len, r); }
    LogHex(L"send", buf, len);
    return r;
}
int WINAPI Hook_recvfrom(UINT_PTR s, char* buf, int len, int flags, BYTE* sa, int* salen) {
    int r = Real_recvfrom(s, buf, len, flags, sa, salen);
    if (r > 0 && g_netLogBudget > 0) {
        --g_netLogBudget;
        wchar_t a[64]; FormatAddr(sa, a, 64);
        Log(L"Net: recvfrom(sock=%u) %d bytes from %s", (unsigned)s, r, a);
    }
    if (r > 0) LogHex(L"recvfrom", buf, r);
    return r;
}
int WINAPI Hook_recv(UINT_PTR s, char* buf, int len, int flags) {
    int r = Real_recv(s, buf, len, flags);
    if (r > 0 && g_netLogBudget > 0) { --g_netLogBudget; Log(L"Net: recv(sock=%u) %d bytes", (unsigned)s, r); }
    if (r > 0) LogHex(L"recv", buf, r);
    return r;
}

}  // namespace

void InitNetSpy(HMODULE exe) {
    HookSpec hooks[] = {
        {"kernel32.dll", "OutputDebugStringW", (void*)Hook_OutputDebugStringW, (void**)&Real_OutputDebugStringW},
        {"kernel32.dll", "OutputDebugStringA", (void*)Hook_OutputDebugStringA, (void**)&Real_OutputDebugStringA},
        // ws2_32 ordinals: 23 socket, 2 bind, 4 connect, 20 sendto, 19 send, 17 recvfrom, 16 recv
        {"ws2_32.dll", MAKEINTRESOURCEA(23), (void*)Hook_socket, (void**)&Real_socket},
        {"ws2_32.dll", MAKEINTRESOURCEA(2), (void*)Hook_bind, (void**)&Real_bind},
        {"ws2_32.dll", MAKEINTRESOURCEA(4), (void*)Hook_connect, (void**)&Real_connect},
        {"ws2_32.dll", MAKEINTRESOURCEA(20), (void*)Hook_sendto, (void**)&Real_sendto},
        {"ws2_32.dll", MAKEINTRESOURCEA(19), (void*)Hook_send, (void**)&Real_send},
        {"ws2_32.dll", MAKEINTRESOURCEA(17), (void*)Hook_recvfrom, (void**)&Real_recvfrom},
        {"ws2_32.dll", MAKEINTRESOURCEA(16), (void*)Hook_recv, (void**)&Real_recv},
    };
    InstallHooks(exe, hooks);
}
