#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "cpu.h"
#include "nb.h"

namespace nb {

// ---------------------------------------------------------------- logging

void logf(int prio, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write(prio, "ninbridge", buf);
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write(LOG_ERROR, "ninbridge", buf);
    abort();
}

// ---------------------------------------------------------------- handler registry

namespace {

struct Import {
    std::string name;
    Handler fn;
};

std::mutex g_reg_mu;
// index = svc number - kSvcThunkBase. Fixed-size so CallSVC can read it without locking.
constexpr u32 kMaxStubs = 4096;
Import g_handlers[kMaxStubs];
std::atomic<u32> g_handler_count{0};
std::unordered_map<std::string, Handler> g_by_name;
std::unordered_map<std::string, gaddr> g_data;

constexpr u32 kArmBxLr = 0xE12FFF1E;
constexpr u32 kArmSvc = 0xEF000000;
constexpr u32 kArmDmbIsh = 0xF57FF05B;

}  // namespace

void reg(const char* name, Handler fn) {
    std::lock_guard lk(g_reg_mu);
    g_by_name[name] = fn;
}

void reg_data(const char* name, gaddr addr) {
    std::lock_guard lk(g_reg_mu);
    g_data[name] = addr;
}

Handler find_handler(const char* name) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_by_name.find(name);
    return it == g_by_name.end() ? nullptr : it->second;
}

gaddr find_data(const char* name) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_data.find(name);
    return it == g_data.end() ? 0 : it->second;
}

gaddr make_stub(const char* name, Handler fn) {
    u32 idx;
    {
        std::lock_guard lk(g_reg_mu);
        idx = g_handler_count.load(std::memory_order_relaxed);
        if (idx >= kMaxStubs) fatal("too many stubs");
        g_handlers[idx] = {name, fn};
        g_handler_count.store(idx + 1, std::memory_order_release);
    }
    gaddr a = misc_alloc(8, 8);
    wr<u32>(a, kArmSvc | (kSvcThunkBase + idx));
    wr<u32>(a + 4, kArmBxLr);
    return a;
}

static gaddr g_return_trap = 0;

// ---------------------------------------------------------------- CP15

namespace {

// Minimal CP15: barrier operations and the user read-only thread ID register (TPIDRURO).
using Dynarmic::A32::CoprocReg;

class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(u32* tls) : tls_(tls) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        // c7 barriers: CP15ISB (c5,4), CP15DSB (c10,4), CP15DMB (c10,5)
        if (!two && opc1 == 0 && CRn == CoprocReg::C7) {
            if ((CRm == CoprocReg::C5 && opc2 == 4) || (CRm == CoprocReg::C10 && (opc2 == 4 || opc2 == 5))) {
                return Callback{[](void*, u32, u32) -> u64 {
                                    std::atomic_thread_fence(std::memory_order_seq_cst);
                                    return 0;
                                },
                                std::nullopt};
            }
        }
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 3) return tls_;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<u8>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<u8>) override { return std::nullopt; }

private:
    u32* tls_;
};

// ---------------------------------------------------------------- processor ids for the monitor

constexpr size_t kMaxCpus = 512;
Dynarmic::ExclusiveMonitor* g_monitor;
std::mutex g_pid_mu;
std::vector<size_t> g_free_pids;
size_t g_next_pid = 0;

size_t alloc_pid() {
    std::lock_guard lk(g_pid_mu);
    if (!g_free_pids.empty()) {
        size_t p = g_free_pids.back();
        g_free_pids.pop_back();
        return p;
    }
    if (g_next_pid >= kMaxCpus) fatal("too many concurrent guest CPUs");
    return g_next_pid++;
}

void free_pid(size_t p) {
    std::lock_guard lk(g_pid_mu);
    g_monitor->ClearProcessor(p);
    g_free_pids.push_back(p);
}

}  // namespace

// ---------------------------------------------------------------- Cpu

Cpu::Cpu(GuestThread* t, int depth) : thread(t), depth(depth), pid(alloc_pid()) {
    tls_word = t->tls;
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.processor_id = pid;
    cfg.global_monitor = g_monitor;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
    cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(g_base);
    cfg.enable_cycle_counting = false;
    // The guest never modifies its own code, and the range index costs ~100+ MB of host heap.
    cfg.track_code_ranges = false;
    // Every thread has its own JIT (dynarmic is not re-entrant); nested ones run little code.
    cfg.code_cache_size = depth == 0 ? 128u * 1024 * 1024 : 16u * 1024 * 1024;
    cfg.coprocessors[15] = std::make_shared<Cp15>(&tls_word);
    jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

Cpu::~Cpu() {
    jit.reset();
    free_pid(pid);
}

void Cpu::check(gaddr a, u32 n, const char* what) {
    if (!is_mapped(a) || !is_mapped(a + n - 1)) {
        dump("bad memory access");
        fatal("guest %s of %u bytes at unmapped address %#x", what, n, a);
    }
}

std::optional<u32> Cpu::MemoryReadCode(u32 a) {
    if (!is_mapped(a)) return std::nullopt;
    return rd<u32>(a);
}
u8 Cpu::MemoryRead8(u32 a) { check(a, 1, "read"); return rd<u8>(a); }
u16 Cpu::MemoryRead16(u32 a) { check(a, 2, "read"); return rd<u16>(a); }
u32 Cpu::MemoryRead32(u32 a) { check(a, 4, "read"); return rd<u32>(a); }
u64 Cpu::MemoryRead64(u32 a) { check(a, 8, "read"); return rd<u64>(a); }
void Cpu::MemoryWrite8(u32 a, u8 v) { check(a, 1, "write"); wr(a, v); }
void Cpu::MemoryWrite16(u32 a, u16 v) { check(a, 2, "write"); wr(a, v); }
void Cpu::MemoryWrite32(u32 a, u32 v) { check(a, 4, "write"); wr(a, v); }
void Cpu::MemoryWrite64(u32 a, u64 v) { check(a, 8, "write"); wr(a, v); }

template <class T>
static bool cas(gaddr a, T v, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(g_base + a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
bool Cpu::MemoryWriteExclusive8(u32 a, u8 v, u8 e) { check(a, 1, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive16(u32 a, u16 v, u16 e) { check(a, 2, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive32(u32 a, u32 v, u32 e) { check(a, 4, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive64(u32 a, u64 v, u64 e) { check(a, 8, "strexd"); return cas(a, v, e); }

void Cpu::InterpreterFallback(u32 pc, size_t n) {
    dump("interpreter fallback");
    fatal("dynarmic requested interpreter fallback at %#x (%zu instrs)", pc, n);
}

void Cpu::ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::SendEvent:
    case E::SendEventLocal:
    case E::WaitForInterrupt:
    case E::WaitForEvent:
    case E::Yield:
    case E::PreloadData:
    case E::PreloadDataWithIntentToWrite:
    case E::PreloadInstruction:
        return;
    default:
        break;
    }
    static const char* names[] = {"UndefinedInstruction", "UnpredictableInstruction", "DecodeError", "SendEvent",
                                  "SendEventLocal", "WaitForInterrupt", "WaitForEvent", "Yield", "Breakpoint",
                                  "PreloadData", "PreloadDataWithIntentToWrite", "PreloadInstruction", "NoExecuteFault"};
    jit->Regs()[15] = pc;
    dump("cpu exception");
    u32 insn = is_mapped(pc) ? rd<u32>(pc) : 0;
    fatal("guest exception %s at %#x [%s] insn=%08x", names[size_t(e)], pc, symbolize(pc).c_str(), insn);
}

void Cpu::dump(const char* why) {
    auto& r = jit->Regs();
    NB_LOGE("=== guest state (%s), thread %u depth %d ===", why, thread->tid, depth);
    for (int i = 0; i < 16; i += 4)
        NB_LOGE("r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3,
                r[i + 3]);
    NB_LOGE("cpsr %08x", jit->Cpsr());
    NB_LOGE("pc %s", symbolize(r[15]).c_str());
    NB_LOGE("lr %s", symbolize(r[14]).c_str());
    // Heuristic backtrace: code addresses found on the stack.
    gaddr sp = r[13];
    int found = 0;
    for (gaddr p = sp; p < sp + 1024 && found < 12 && is_mapped(p); p += 4) {
        u32 v = rd<u32>(p);
        if (v >= layout::kLibBase && v < layout::kLibBase + 0x1b40000 && v != r[14]) {
            NB_LOGE("  stack[%#x] %s", p - sp, symbolize(v).c_str());
            ++found;
        }
    }
}

// ---------------------------------------------------------------- SVC dispatch

static void do_syscall(Cpu& cpu) {
    auto& r = cpu.jit->Regs();
    u32 nr = r[7];
    switch (nr) {
    case 0xf0002:  // __ARM_NR_cacheflush
        cpu.jit->InvalidateCacheRange(r[0], r[1] - r[0]);
        r[0] = 0;
        return;
    case 20:  // getpid
        r[0] = u32(getpid());
        return;
    case 224:  // gettid
        r[0] = cpu.thread->tid;
        return;
    default:
        cpu.dump("raw syscall");
        NB_LOGW("unsupported raw syscall %u", nr);
        r[0] = u32(-ENOSYS);
    }
}

void Cpu::CallSVC(u32 swi) {
    auto& r = jit->Regs();
    if (swi >= kSvcThunkBase) {
        u32 idx = swi - kSvcThunkBase;
        if (idx >= g_handler_count.load(std::memory_order_acquire)) fatal("bad thunk svc %#x", swi);
        const Import* imp = &g_handlers[idx];
        Ctx c(*this, imp->name.c_str());
        imp->fn(c);
        return;
    }
    switch (swi) {
    case kSvcReturn:
        returned = true;
        jit->HaltExecution();
        return;
    case kSvcKuserCmpxchg: {
        bool ok = cas<u32>(r[2], r[1], r[0]);
        r[0] = ok ? 0 : 1;
        u32 cpsr = jit->Cpsr();
        jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
        return;
    }
    case kSvcKuserCmpxchg64: {
        u64 oldv = rd<u64>(r[0]), newv = rd<u64>(r[1]);
        bool ok = cas<u64>(r[2], newv, oldv);
        r[0] = ok ? 0 : 1;
        u32 cpsr = jit->Cpsr();
        jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
        return;
    }
    case kSvcKuserGetTls:
        r[0] = thread->tls;
        return;
    case 0:
        do_syscall(*this);
        return;
    default:
        dump("unknown svc");
        fatal("unknown svc #%#x", swi);
    }
}

Ctx::Ctx(Cpu& c, const char* nm) : cpu(c), r(c.jit->Regs()), name(nm), nsaa(c.jit->Regs()[13]) {}
GuestThread& Ctx::thread() { return *cpu.thread; }

// ---------------------------------------------------------------- threads

namespace {
std::atomic<u32> g_next_tid{1};
thread_local std::unique_ptr<GuestThread> t_thread;
}  // namespace

GuestThread::GuestThread() {
    tid = g_next_tid++;
    stack_size = 8u << 20;
    stack_base = map_alloc(stack_size + kPageSize, PROT_NONE);
    if (!stack_base) fatal("out of guest memory for a stack");
    // lowest page stays inaccessible as a guard
    mem_protect(stack_base + kPageSize, stack_size, PROT_READ | PROT_WRITE);
    stack_base += kPageSize;
    tls = map_alloc(tlsoff::kSize, PROT_READ | PROT_WRITE);
}

GuestThread::~GuestThread() {
    cpus.clear();
    map_free(stack_base - kPageSize, stack_size + kPageSize);
    map_free(tls, tlsoff::kSize);
}

GuestThread& cur_thread() {
    if (!t_thread) t_thread = std::make_unique<GuestThread>();
    return *t_thread;
}

void set_guest_errno(int e) { wr<s32>(cur_thread().tls + tlsoff::kErrno, e); }

// ---------------------------------------------------------------- calls

u64 call_guest(gaddr fn, std::initializer_list<GArg> args) { return call_guest(fn, args.begin(), args.size()); }

u64 call_guest(gaddr fn, const GArg* args, size_t n) {
    if (!fn) fatal("call_guest: null function");
    GuestThread& t = cur_thread();
    int d = ++t.depth;
    if (int(t.cpus.size()) <= d) t.cpus.push_back(std::make_unique<Cpu>(&t, d));
    Cpu& c = *t.cpus[d];

    gaddr sp = d == 0 ? t.stack_base + t.stack_size : t.cpus[d - 1]->jit->Regs()[13];
    sp = (sp - 256) & ~7u;

    // AAPCS argument assignment.
    u32 regs[4] = {};
    int ncrn = 0;
    std::vector<std::pair<u32, u32>> stacked;  // (offset, word)
    u32 nsaa = 0;
    for (size_t i = 0; i < n; ++i) {
        if (args[i].wide) {
            ncrn += ncrn & 1;
            if (ncrn <= 2) {
                regs[ncrn++] = u32(args[i].v);
                regs[ncrn++] = u32(args[i].v >> 32);
            } else {
                ncrn = 4;
                nsaa = (nsaa + 7) & ~7u;
                stacked.push_back({nsaa, u32(args[i].v)});
                stacked.push_back({nsaa + 4, u32(args[i].v >> 32)});
                nsaa += 8;
            }
        } else if (ncrn < 4) {
            regs[ncrn++] = u32(args[i].v);
        } else {
            stacked.push_back({nsaa, u32(args[i].v)});
            nsaa += 4;
        }
    }
    sp = (sp - nsaa) & ~7u;
    for (auto [off, w] : stacked) wr<u32>(sp + off, w);

    auto& R = c.jit->Regs();
    for (int i = 0; i < 4; ++i) R[i] = regs[i];
    R[13] = sp;
    R[14] = g_return_trap;
    R[15] = fn & ~1u;
    c.jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));  // USR mode, ARM/Thumb per target
    c.jit->SetFpscr(0);
    c.jit->ClearExclusiveState();
    c.returned = false;
    while (!c.returned && !t.exiting) c.jit->Run();
    u64 res = R[0] | (u64(R[1]) << 32);
    --t.depth;
    return res;
}

// ---------------------------------------------------------------- setup

void cpu_init() {
    g_monitor = new Dynarmic::ExclusiveMonitor(kMaxCpus);

    g_return_trap = misc_alloc(8, 8);
    wr<u32>(g_return_trap, kArmSvc | kSvcReturn);
    wr<u32>(g_return_trap + 4, kArmBxLr);

    // ARM Linux kernel user helpers (used by libgcc's __sync_* implementation).
    using layout::kKuserPage;
    mem_protect(kKuserPage, kPageSize, PROT_READ | PROT_WRITE);
    wr<u32>(kKuserPage + 0xf60, kArmSvc | kSvcKuserCmpxchg64);
    wr<u32>(kKuserPage + 0xf64, kArmBxLr);
    wr<u32>(kKuserPage + 0xfa0, kArmDmbIsh);
    wr<u32>(kKuserPage + 0xfa4, kArmBxLr);
    wr<u32>(kKuserPage + 0xfc0, kArmSvc | kSvcKuserCmpxchg);
    wr<u32>(kKuserPage + 0xfc4, kArmBxLr);
    wr<u32>(kKuserPage + 0xfe0, kArmSvc | kSvcKuserGetTls);
    wr<u32>(kKuserPage + 0xfe4, kArmBxLr);
    wr<u32>(kKuserPage + 0xffc, 5);  // __kuser_helper_version
}

}  // namespace nb
