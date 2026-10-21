// stdio for the guest: FILE handles, printf and scanf families.
#include <unistd.h>
#include <wchar.h>

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "nb.h"

#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat-security"

namespace nb {

namespace {

// Old bionic's struct __sFILE is 84 bytes; _flags (short) at +12 and _file (short) at +14.
constexpr u32 kFileSize = 84;
constexpr u16 kSEOF = 0x0020, kSERR = 0x0040;

struct GFile {
    FILE* f = nullptr;
    int log_prio = 0;  // non-zero: stdout/stderr routed to the log
    std::string pending;
};

std::mutex g_files_mu;
std::unordered_map<gaddr, GFile*> g_files;
gaddr g_sF = 0;

GFile* lookup(gaddr gf) {
    std::lock_guard lk(g_files_mu);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}

gaddr wrap_file(FILE* f) {
    gaddr gf = gcalloc(1, kFileSize);
    wr<s16>(gf + 14, s16(fileno(f)));
    std::lock_guard lk(g_files_mu);
    g_files[gf] = new GFile{f, 0, {}};
    return gf;
}

void sync_flags(gaddr gf, GFile* f) {
    if (!f->f) return;
    u16 fl = rd<u16>(gf + 12) & ~(kSEOF | kSERR);
    if (feof(f->f)) fl |= kSEOF;
    if (ferror(f->f)) fl |= kSERR;
    wr<u16>(gf + 12, fl);
}

void log_out(GFile* f, const char* data, size_t n) {
    f->pending.append(data, n);
    size_t pos;
    while ((pos = f->pending.find('\n')) != std::string::npos) {
        log_write(f->log_prio, "nin", f->pending.substr(0, pos).c_str());
        f->pending.erase(0, pos + 1);
    }
    if (f->pending.size() > 4000) {
        log_write(f->log_prio, "nin", f->pending.c_str());
        f->pending.clear();
    }
}

// Writes n bytes; returns bytes written.
size_t file_write(gaddr gf, const void* data, size_t n) {
    GFile* f = lookup(gf);
    if (!f) {
        set_guest_errno(EBADF);
        return 0;
    }
    if (f->log_prio) {
        log_out(f, static_cast<const char*>(data), n);
        return n;
    }
    size_t r = fwrite(data, 1, n, f->f);
    sync_flags(gf, f);
    return r;
}

template <class T>
void append_fmt(std::string& out, const std::string& spec, T v) {
    char small[128];
    int n = snprintf(small, sizeof small, spec.c_str(), v);
    if (n < 0) return;
    if (size_t(n) < sizeof small) {
        out.append(small, size_t(n));
        return;
    }
    size_t o = out.size();
    out.resize(o + size_t(n) + 1);
    snprintf(&out[o], size_t(n) + 1, spec.c_str(), v);
    out.resize(o + size_t(n));
}

enum Len { kNone, kHH, kH, kL, kLL, kBigL, kJ, kZ, kT };

Len parse_len(const char*& p) {
    switch (*p) {
    case 'h':
        if (p[1] == 'h') {
            p += 2;
            return kHH;
        }
        ++p;
        return kH;
    case 'l':
        if (p[1] == 'l') {
            p += 2;
            return kLL;
        }
        ++p;
        return kL;
    case 'q': ++p; return kLL;
    case 'L': ++p; return kBigL;
    case 'j': ++p; return kJ;
    case 'z': ++p; return kZ;
    case 't': ++p; return kT;
    default: return kNone;
    }
}

}  // namespace

std::string gformat(const char* fmt, ArgSrc& a) {
    std::string out;
    if (!fmt) return out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            const char* q = p;
            while (*q && *q != '%') ++q;
            out.append(p, size_t(q - p));
            p = q;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            ++p;
            continue;
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(s32(a.next32()));
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                s32 pr = s32(a.next32());
                if (pr >= 0) spec += "." + std::to_string(pr);
                ++p;
            } else {
                spec += '.';
                while (*p >= '0' && *p <= '9') spec += *p++;
            }
        }
        Len len = parse_len(p);
        char conv = *p ? *p++ : 0;
        switch (conv) {
        case 'd':
        case 'i': {
            s64 v = (len == kLL || len == kJ) ? s64(a.next64()) : s64(s32(a.next32()));
            if (len == kHH) v = s8(v);
            if (len == kH) v = s16(v);
            append_fmt(out, spec + "lld", static_cast<long long>(v));
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = (len == kLL || len == kJ) ? a.next64() : u64(a.next32());
            if (len == kHH) v = u8(v);
            if (len == kH) v = u16(v);
            append_fmt(out, spec + "ll" + conv, static_cast<unsigned long long>(v));
            break;
        }
        case 'c':
            if (len == kL) {
                append_fmt(out, spec + "lc", wint_t(a.next32()));
            } else {
                append_fmt(out, spec + "c", int(a.next32()));
            }
            break;
        case 's': {
            gaddr s = a.next32();
            if (len == kL) {
                append_fmt(out, spec + "ls", s ? hp<const wchar_t>(s) : L"(null)");
            } else {
                append_fmt(out, spec + "s", s ? hs(s) : "(null)");
            }
            break;
        }
        case 'p': {
            char tmp[16];
            snprintf(tmp, sizeof tmp, "0x%x", a.next32());
            append_fmt(out, spec + "s", tmp);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            u64 b = a.next64();
            double d;
            memcpy(&d, &b, 8);
            append_fmt(out, spec + conv, d);
            break;
        }
        case 'n': {
            gaddr ptr = a.next32();
            if (!ptr) break;
            u32 n = u32(out.size());
            if (len == kHH)
                wr<u8>(ptr, u8(n));
            else if (len == kH)
                wr<u16>(ptr, u16(n));
            else if (len == kLL || len == kJ)
                wr<u64>(ptr, n);
            else
                wr<u32>(ptr, n);
            break;
        }
        default:
            out.append(start, size_t(p - start));
            break;
        }
    }
    return out;
}

int gsscanf(const char* input, const char* fmt, ArgSrc& a) {
    const char* in = input;
    int assigned = 0;
    bool converted = false;
    const char* p = fmt;
    while (*p) {
        std::string chunk;
        while (*p && !(p[0] == '%' && p[1] != '%')) {
            if (p[0] == '%') {
                chunk += "%%";
                p += 2;
            } else {
                chunk += *p++;
            }
        }
        if (!*p) break;  // trailing literal text does not affect the result
        ++p;
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            ++p;
        }
        std::string width;
        while (*p >= '0' && *p <= '9') width += *p++;
        Len len = parse_len(p);
        char conv = *p ? *p++ : 0;
        std::string set;
        if (conv == '[') {
            set = "[";
            if (*p == '^') set += *p++;
            if (*p == ']') set += *p++;
            while (*p && *p != ']') set += *p++;
            if (*p == ']') set += *p++;
        }
        std::string head = chunk + "%" + (suppress ? "*" : "") + width;
        int consumed = -1;
        if (conv == 'n') {
            sscanf(in, (chunk + "%n").c_str(), &consumed);
            if (consumed < 0) break;
            in += consumed;
            gaddr ptr = a.next32();
            if (!suppress && ptr) wr<s32>(ptr, s32(in - input));
            continue;
        }
        gaddr dst = suppress ? 0 : a.next32();
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            long long v = 0;
            char c2 = conv == 'p' ? 'x' : conv;
            if (suppress)
                sscanf(in, (head + c2 + "%n").c_str(), &consumed);
            else
                sscanf(in, (head + "ll" + c2 + "%n").c_str(), &v, &consumed);
            if (consumed < 0) break;
            if (dst) {
                if (len == kHH)
                    wr<u8>(dst, u8(v));
                else if (len == kH)
                    wr<u16>(dst, u16(v));
                else if ((len == kLL || len == kJ) && conv != 'p')
                    wr<u64>(dst, u64(v));
                else
                    wr<u32>(dst, u32(v));
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            double v = 0;
            if (suppress)
                sscanf(in, (head + conv + "%n").c_str(), &consumed);
            else
                sscanf(in, (head + "l" + conv + "%n").c_str(), &v, &consumed);
            if (consumed < 0) break;
            if (dst) {
                if (len == kL || len == kBigL)
                    wr<double>(dst, v);
                else
                    wr<float>(dst, float(v));
            }
            break;
        }
        case 's':
        case 'c':
        case '[': {
            std::string spec = head + (len == kL ? "l" : "") + (conv == '[' ? set : std::string(1, conv)) + "%n";
            if (suppress)
                sscanf(in, spec.c_str(), &consumed);
            else
                sscanf(in, spec.c_str(), h(dst), &consumed);
            break;
        }
        default:
            NB_LOGW("sscanf: unsupported conversion '%c'", conv);
            break;
        }
        if (consumed < 0) {
            if (!converted) {
                const char* q = in;
                while (*q == ' ' || (*q >= '\t' && *q <= '\r')) ++q;
                if (!*q) return -1;  // input failure before the first conversion
            }
            break;
        }
        converted = true;
        in += consumed;
        if (!suppress) ++assigned;
    }
    return assigned;
}

namespace {

// ------------------------------------------------------------------ FILE functions

u32 x_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (!f) {
        errno_from_host();
        return 0;
    }
    return wrap_file(f);
}
u32 x_fdopen(int fd, const char* mode) {
    FILE* f = fdopen(fd, mode);
    if (!f) {
        errno_from_host();
        return 0;
    }
    return wrap_file(f);
}
u32 x_freopen(const char* path, const char* mode, u32 gf) {
    GFile* f = lookup(gf);
    if (!f || f->log_prio) {
        set_guest_errno(EBADF);
        return 0;
    }
    FILE* nf = freopen(path, mode, f->f);
    if (!nf) {
        errno_from_host();
        return 0;
    }
    f->f = nf;
    wr<s16>(gf + 14, s16(fileno(nf)));
    return gf;
}
int x_fclose(u32 gf) {
    GFile* f;
    {
        std::lock_guard lk(g_files_mu);
        auto it = g_files.find(gf);
        if (it == g_files.end()) {
            set_guest_errno(EBADF);
            return -1;
        }
        f = it->second;
        if (gf >= g_sF && gf < g_sF + 3 * kFileSize) return 0;  // never close std streams
        g_files.erase(it);
    }
    int r = fclose(f->f);
    if (r) errno_from_host();
    delete f;
    gfree(gf);
    return r;
}
int x_fflush(u32 gf) {
    if (!gf) return fflush(nullptr);
    GFile* f = lookup(gf);
    if (!f) return 0;
    if (f->log_prio) return 0;
    return fflush(f->f);
}
int x_fgetc(u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return -1;
    int r = fgetc(f->f);
    sync_flags(gf, f);
    return r;
}
int x_ungetc(int ch, u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return -1;
    int r = ungetc(ch, f->f);
    sync_flags(gf, f);
    return r;
}
u32 x_fgets(u32 buf, int n, u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return 0;
    char* r = fgets(hp<char>(buf), n, f->f);
    sync_flags(gf, f);
    return r ? buf : 0;
}
int x_fputc(int ch, u32 gf) {
    char c = char(ch);
    return file_write(gf, &c, 1) == 1 ? (ch & 0xff) : -1;
}
int x_fputs(const char* s, u32 gf) {
    size_t n = strlen(s);
    return file_write(gf, s, n) == n ? 1 : -1;
}
u32 x_fread(void* ptr, u32 size, u32 n, u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f || !size) return 0;
    size_t r = fread(ptr, size, n, f->f);
    sync_flags(gf, f);
    return u32(r);
}
u32 x_fwrite(const void* ptr, u32 size, u32 n, u32 gf) {
    if (!size || !n) return 0;
    return u32(file_write(gf, ptr, size_t(size) * n) / size);
}
int x_fseek(u32 gf, s32 off, int whence) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return -1;
    int r = fseek(f->f, off, whence);
    if (r) errno_from_host();
    sync_flags(gf, f);
    return r;
}
s32 x_ftell(u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return -1;
    long r = ftell(f->f);
    if (r < 0) errno_from_host();
    return s32(r);
}
int x_setvbuf(u32 gf, u32, int mode, u32 size) {
    GFile* f = lookup(gf);
    if (!f || !f->f || f->log_prio) return 0;
    return setvbuf(f->f, nullptr, mode, size);  // let the host own the buffer
}
u32 x_getwc(u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return u32(WEOF);
    wint_t r = fgetwc(f->f);
    sync_flags(gf, f);
    return r;
}
u32 x_putwc(u32 wc, u32 gf) {
    char buf[8];
    mbstate_t st{};
    size_t n = wcrtomb(buf, wchar_t(wc), &st);
    if (n == size_t(-1)) return u32(WEOF);
    return file_write(gf, buf, n) == n ? wc : u32(WEOF);
}
u32 x_ungetwc(u32 wc, u32 gf) {
    GFile* f = lookup(gf);
    if (!f || !f->f) return u32(WEOF);
    return ungetwc(wint_t(wc), f->f);
}

// ------------------------------------------------------------------ formatted I/O

void x_printf(Ctx& c) {
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    c.ret(u32(file_write(g_sF + kFileSize, s.data(), s.size())));
}
void x_fprintf(Ctx& c) {
    gaddr gf = c.a32();
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    c.ret(u32(file_write(gf, s.data(), s.size())));
}
void x_sprintf(Ctx& c) {
    gaddr buf = c.a32();
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    memcpy(h(buf), s.c_str(), s.size() + 1);
    c.ret(u32(s.size()));
}
void x_snprintf(Ctx& c) {
    gaddr buf = c.a32();
    u32 n = c.a32();
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    if (n) {
        size_t k = s.size() < n - 1 ? s.size() : n - 1;
        memcpy(h(buf), s.data(), k);
        wr<u8>(buf + u32(k), 0);
    }
    c.ret(u32(s.size()));
}
void x_vsprintf(Ctx& c) {
    gaddr buf = c.a32();
    const char* fmt = hs(c.a32());
    VaArgs a(c.a32());
    std::string s = gformat(fmt, a);
    memcpy(h(buf), s.c_str(), s.size() + 1);
    c.ret(u32(s.size()));
}
void x_sscanf(Ctx& c) {
    const char* in = hs(c.a32());
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    c.ret(u32(gsscanf(in, fmt, a)));
}

}  // namespace

void register_stdio() {
    g_sF = misc_alloc(3 * kFileSize, 8);
    {
        std::lock_guard lk(g_files_mu);
        g_files[g_sF] = new GFile{stdin, 0, {}};
        g_files[g_sF + kFileSize] = new GFile{nullptr, LOG_INFO, {}};
        g_files[g_sF + 2 * kFileSize] = new GFile{nullptr, LOG_WARN, {}};
    }
    for (int i = 0; i < 3; ++i) wr<s16>(g_sF + i * kFileSize + 14, s16(i));
    reg_data("__sF", g_sF);

    NB_REG("fopen", x_fopen);
    NB_REG("fdopen", x_fdopen);
    NB_REG("freopen", x_freopen);
    NB_REG("fclose", x_fclose);
    NB_REG("fflush", x_fflush);
    NB_REG("fgetc", x_fgetc);
    NB_REG("getc", x_fgetc);
    NB_REG("ungetc", x_ungetc);
    NB_REG("fgets", x_fgets);
    NB_REG("fputc", x_fputc);
    NB_REG("putc", x_fputc);
    NB_REG("fputs", x_fputs);
    NB_REG("fread", x_fread);
    NB_REG("fwrite", x_fwrite);
    NB_REG("fseek", x_fseek);
    NB_REG("ftell", x_ftell);
    NB_REG("setvbuf", x_setvbuf);
    NB_REG("getwc", x_getwc);
    NB_REG("putwc", x_putwc);
    NB_REG("ungetwc", x_ungetwc);
    reg("printf", x_printf);
    reg("fprintf", x_fprintf);
    reg("sprintf", x_sprintf);
    reg("snprintf", x_snprintf);
    reg("vsprintf", x_vsprintf);
    reg("sscanf", x_sscanf);
}

}  // namespace nb
