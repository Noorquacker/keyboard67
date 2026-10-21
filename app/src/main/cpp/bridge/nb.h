// ninbridge: runs the original 32-bit ARM libgl2jni.so inside a 64-bit process.
//
// The guest library is loaded into a private 4 GiB "guest address space" and executed by the
// dynarmic ARM32 JIT. Every function it imports (libc, GLES2, liblog, libandroid, JNI) is bound
// to a tiny ARM stub ("svc #n; bx lr") whose SVC lands in a host handler that marshals the
// AAPCS (soft-float) arguments, translates guest pointers and calls the real host function.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace nb {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using gaddr = uint32_t;  // a pointer in guest address space

// ---------------------------------------------------------------- logging

enum { LOG_DEBUG = 3, LOG_INFO = 4, LOG_WARN = 5, LOG_ERROR = 6 };
void logf(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_write(int prio, const char* tag, const char* msg);
#define NB_LOGI(...) ::nb::logf(::nb::LOG_INFO, __VA_ARGS__)
#define NB_LOGW(...) ::nb::logf(::nb::LOG_WARN, __VA_ARGS__)
#define NB_LOGE(...) ::nb::logf(::nb::LOG_ERROR, __VA_ARGS__)

// ---------------------------------------------------------------- guest memory

namespace layout {
constexpr gaddr kMiscBase = 0x00100000;  // stubs, import data, JNI tables
constexpr u32 kMiscSize = 0x00400000;
constexpr gaddr kLibBase = 0x01000000;  // the guest .so
constexpr gaddr kHeapBase = 0x08000000;  // dlmalloc sbrk arena
constexpr gaddr kHeapEnd = 0x90000000;
constexpr gaddr kMapBase = 0x90000000;  // mmap(), thread stacks, TLS blocks
constexpr gaddr kMapEnd = 0xfe000000;
constexpr gaddr kKuserPage = 0xffff0000;  // ARM Linux kernel user helpers
}  // namespace layout

constexpr u32 kPageSize = 4096;
inline u32 page_round_up(u32 v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

extern u8* g_base;
extern u32 g_host_page;  // 4 KiB or 16 KiB
u32 host_page_round_up(u32 v);

inline void* h(gaddr a) { return a ? g_base + a : nullptr; }
template <class T>
inline T* hp(gaddr a) { return reinterpret_cast<T*>(h(a)); }
inline const char* hs(gaddr a) { return reinterpret_cast<const char*>(h(a)); }
inline bool in_guest(const void* p) {
    return reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(g_base) < (1ull << 32);
}
gaddr g(const void* p);  // host -> guest pointer; fatal if outside guest space

template <class T>
inline T rd(gaddr a) {
    T v;
    std::memcpy(&v, g_base + a, sizeof v);
    return v;
}
template <class T>
inline void wr(gaddr a, T v) {
    std::memcpy(g_base + a, &v, sizeof v);
}

void mem_init();
bool is_mapped(gaddr a);
void mem_protect(gaddr a, u32 size, int prot);  // also maintains the mapped-page bitmap
gaddr misc_alloc(u32 size, u32 align = 8);      // permanent bump allocation in the misc area
gaddr map_alloc(u32 size, int prot);            // page-granular, from the map area
bool map_claim(gaddr a, u32 size);              // claim a fixed range of the map area
void map_free(gaddr a, u32 size);

// guest heap (dlmalloc over a guest sbrk arena)
gaddr gmalloc(u32 n);
gaddr gcalloc(u32 n, u32 sz);
gaddr grealloc(gaddr p, u32 n);
void gfree(gaddr p);
gaddr gstrdup(const char* s);
gaddr gmemdup(const void* p, u32 n);

// ---------------------------------------------------------------- CPU / calls

class Cpu;
struct GuestThread;

// Argument cursor following the AAPCS base (soft-float) procedure call standard.
struct Ctx {
    Cpu& cpu;
    std::array<u32, 16>& r;
    const char* name;  // name of the import being serviced (diagnostics)
    int ncrn = 0;      // next core register number
    gaddr nsaa;        // next stacked argument address

    Ctx(Cpu& c, const char* nm);

    u32 a32() {
        if (ncrn < 4) return r[ncrn++];
        u32 v = rd<u32>(nsaa);
        nsaa += 4;
        return v;
    }
    u64 a64() {
        ncrn += ncrn & 1;
        if (ncrn <= 2) {
            u64 v = r[ncrn] | (u64(r[ncrn + 1]) << 32);
            ncrn += 2;
            return v;
        }
        ncrn = 4;
        nsaa = (nsaa + 7) & ~7u;
        u64 v = rd<u64>(nsaa);
        nsaa += 8;
        return v;
    }
    void ret(u32 v) { r[0] = v; }
    void ret64(u64 v) {
        r[0] = u32(v);
        r[1] = u32(v >> 32);
    }
    GuestThread& thread();
};

// Source of variadic arguments: either the live call (Ctx) or a guest va_list.
struct ArgSrc {
    virtual ~ArgSrc() = default;
    virtual u32 next32() = 0;
    virtual u64 next64() = 0;
};
struct CtxArgs final : ArgSrc {
    Ctx& c;
    explicit CtxArgs(Ctx& cc) : c(cc) {}
    u32 next32() override { return c.a32(); }
    u64 next64() override { return c.a64(); }
};
struct VaArgs final : ArgSrc {
    gaddr p;  // ARM AAPCS va_list is a plain pointer into the argument save area
    explicit VaArgs(gaddr ap) : p(ap) {}
    u32 next32() override {
        u32 v = rd<u32>(p);
        p += 4;
        return v;
    }
    u64 next64() override {
        p = (p + 7) & ~7u;
        u64 v = rd<u64>(p);
        p += 8;
        return v;
    }
};

using Handler = void (*)(Ctx&);

// Register a host implementation for an imported symbol.
void reg(const char* name, Handler fn);
void reg_data(const char* name, gaddr addr);
// Create a callable guest stub that invokes `fn` (used for JNI function tables etc).
gaddr make_stub(const char* name, Handler fn);
Handler find_handler(const char* name);
gaddr find_data(const char* name);

struct GArg {
    u64 v;
    bool wide;
};
inline GArg ga(u32 v) { return {v, false}; }
inline GArg ga64(u64 v) { return {v, true}; }
inline GArg gaf(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return {b, false};
}
inline GArg gad(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return {b, true};
}

// Call a guest function on the current host thread. Re-entrant: a host handler may call back
// into the guest (qsort comparators, JNI upcalls that re-enter native code, ...).
u64 call_guest(gaddr fn, std::initializer_list<GArg> args);
u64 call_guest(gaddr fn, const GArg* args, size_t n);

void runtime_init();  // loads the guest library; idempotent and thread-safe

// ---------------------------------------------------------------- threads

struct GuestThread {
    u32 tid = 0;
    gaddr stack_base = 0;
    u32 stack_size = 0;
    gaddr tls = 0;  // per-thread guest block (errno, static result buffers)
    std::vector<std::unique_ptr<Cpu>> cpus;
    int depth = -1;
    std::vector<gaddr> specific;  // pthread_getspecific values
    bool exiting = false;
    u32 exit_value = 0;
    size_t compiled_hint = 0;  // approx. compiled blocks; used to pick the warmest pooled thread

    GuestThread();
    ~GuestThread();
};
GuestThread& cur_thread();
void release_current_guest_thread();  // hand this thread's guest state (warm JIT) to the pool

// Block-profile warm-up (warmup.cpp)
void warmup_precompile();       // precompile blocks recorded by a previous run
void warmup_note_frame();       // called per frame; saves the profile now and then

// Offsets inside GuestThread::tls
namespace tlsoff {
constexpr u32 kErrno = 0;
constexpr u32 kStrerror = 0x10;   // 256
constexpr u32 kTm = 0x110;        // 64
constexpr u32 kNtoa = 0x150;      // 32
constexpr u32 kHostent = 0x200;   // 1536
constexpr u32 kH_errno = 0x800;   // 4
constexpr u32 kMisc = 0x900;      // 1024
constexpr u32 kSize = 0x1000;
}  // namespace tlsoff

void set_guest_errno(int e);

// Virtual file descriptors: in-memory files with fd numbers the kernel never handed out.
constexpr int kVfdBase = 32000, kVfdCount = 256;
inline bool vfd_is(int fd) { return fd >= kVfdBase && fd < kVfdBase + kVfdCount; }
int vfd_create(std::string data);  // -1 when full
s32 vfd_read(int fd, void* buf, u32 n);
s64 vfd_lseek(int fd, s64 off, int whence);
u32 vfd_size(int fd);
int vfd_close(int fd);

// Headless test builds substitute host-dependent files (e.g. /proc/self/stat) with fixed ones.
#ifdef NB_HEADLESS
const char* headless_path(const char* p);
#else
inline const char* headless_path(const char* p) { return p; }
#endif
inline void errno_from_host() { set_guest_errno(errno); }

// ---------------------------------------------------------------- loaded image

gaddr guest_sym(const char* name);
u32 guest_image_id();
bool guest_is_code(gaddr pc);  // exported symbol of the guest library (0 if missing)
std::string symbolize(gaddr pc);
void guest_exidx(gaddr* start, u32* count);

// ---------------------------------------------------------------- automatic marshalling

template <class T>
inline T fetch_arg(Ctx& c) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(h(c.a32()));
    } else if constexpr (std::is_same_v<T, float>) {
        u32 b = c.a32();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    } else if constexpr (std::is_same_v<T, double>) {
        u64 b = c.a64();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported argument type");
        if constexpr (sizeof(T) == 8)
            return static_cast<T>(c.a64());
        else
            return static_cast<T>(c.a32());
    }
}

template <class R>
inline void put_ret(Ctx& c, R v) {
    if constexpr (std::is_pointer_v<R>) {
        c.ret(g(v));
    } else if constexpr (std::is_same_v<R, float>) {
        u32 b;
        std::memcpy(&b, &v, 4);
        c.ret(b);
    } else if constexpr (std::is_same_v<R, double>) {
        u64 b;
        std::memcpy(&b, &v, 8);
        c.ret64(b);
    } else if constexpr (sizeof(R) == 8) {
        c.ret64(static_cast<u64>(v));
    } else {
        c.ret(static_cast<u32>(v));
    }
}

template <class R, class... A>
inline void invoke_auto(R (*f)(A...), Ctx& c) {
    // Braced initialisation guarantees left-to-right evaluation of fetch_arg.
    std::tuple<A...> args{fetch_arg<A>(c)...};
    if constexpr (std::is_void_v<R>) {
        std::apply(f, args);
    } else {
        put_ret<R>(c, std::apply(f, args));
    }
}

// Thunk for a function whose signature uses only guest-width types: ints of <=4 bytes,
// int64/uint64 (8 bytes), float, double and pointers (translated guest->host).
// NOTE: never use host `long`/`size_t`-typed functions directly; they are 8 bytes on the host.
template <auto F>
void auto_thunk(Ctx& c) {
    invoke_auto(F, c);
}

#define NB_REG(name, fn) ::nb::reg(name, &::nb::auto_thunk<fn>)

// Register groups (each lives in its own translation unit).
// printf / scanf engines operating on guest arguments
std::string gformat(const char* fmt, ArgSrc& a);
int gsscanf(const char* input, const char* fmt, ArgSrc& a);

void register_libc();
void register_stdio();
void register_pthread();
void register_gles();
bool gles_take_frame_changed();  // did the frame just rendered differ from the previous one?
void register_android();
void register_jni();

}  // namespace nb
