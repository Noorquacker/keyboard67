#pragma once

#include <memory>
#include <optional>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "nb.h"

namespace nb {

constexpr u32 kSvcReturn = 0x10;
constexpr u32 kSvcKuserCmpxchg = 0x11;
constexpr u32 kSvcKuserGetTls = 0x12;
constexpr u32 kSvcKuserCmpxchg64 = 0x13;
constexpr u32 kSvcThunkBase = 0x100;

class Cpu final : public Dynarmic::A32::UserCallbacks {
public:
    Cpu(GuestThread* t, int depth);
    ~Cpu() override;

    std::unique_ptr<Dynarmic::A32::Jit> jit;
    GuestThread* thread;
    int depth;
    size_t pid;
    bool returned = false;
    u32 tls_word = 0;  // value of TPIDRURO

    void dump(const char* why);

    std::optional<u32> MemoryReadCode(u32 vaddr) override;
    u8 MemoryRead8(u32 vaddr) override;
    u16 MemoryRead16(u32 vaddr) override;
    u32 MemoryRead32(u32 vaddr) override;
    u64 MemoryRead64(u32 vaddr) override;
    void MemoryWrite8(u32 vaddr, u8 value) override;
    void MemoryWrite16(u32 vaddr, u16 value) override;
    void MemoryWrite32(u32 vaddr, u32 value) override;
    void MemoryWrite64(u32 vaddr, u64 value) override;
    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override;
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override;
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override;
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override;
    void InterpreterFallback(u32 pc, size_t num_instructions) override;
    void CallSVC(u32 swi) override;
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return ~0ull >> 1; }

private:
    void check(gaddr a, u32 n, const char* what);
};

void cpu_init();

}  // namespace nb
