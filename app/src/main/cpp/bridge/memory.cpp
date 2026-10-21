#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <map>
#include <string>
#include <mutex>

#include "nb.h"

extern "C" {
void* nbg_malloc(size_t);
void nbg_free(void*);
void* nbg_calloc(size_t, size_t);
void* nbg_realloc(void*, size_t);
}

namespace nb {

u8* g_base = nullptr;

namespace {

// One byte per guest page: non-zero when the page is accessible.
u8* g_page_map = nullptr;

std::mutex g_misc_mu;
gaddr g_misc_next = layout::kMiscBase;

std::mutex g_map_mu;
std::map<gaddr, u32> g_map_free;  // start -> size, coalesced

std::mutex g_brk_mu;
gaddr g_brk = layout::kHeapBase;
gaddr g_brk_committed = layout::kHeapBase;

void set_pages(gaddr a, u32 size, u8 v) {
    for (u64 p = a / kPageSize; p < (u64(a) + size + kPageSize - 1) / kPageSize; ++p) g_page_map[p] = v;
}

}  // namespace

gaddr g(const void* p) {
    if (!p) return 0;
    if (!in_guest(p)) fatal("host pointer %p escaped into guest", p);
    return gaddr(reinterpret_cast<const u8*>(p) - g_base);
}

bool is_mapped(gaddr a) { return g_page_map[a / kPageSize] != 0; }

u32 g_host_page = 4096;

u32 host_page_round_up(u32 v) { return u32((u64(v) + g_host_page - 1) & ~u64(g_host_page - 1)); }

void mem_protect(gaddr a, u32 size, int prot) {
    // Guest pages are 4 KiB but the host may use 16 KiB pages: grant access to every host page
    // touched, and only revoke it from host pages that are entirely inside the range.
    const u64 hp = g_host_page;
    u64 s, e;
    if (prot == PROT_NONE) {
        s = (u64(a) + hp - 1) & ~(hp - 1);
        e = (u64(a) + size) & ~(hp - 1);
    } else {
        s = u64(a) & ~(hp - 1);
        e = (u64(a) + size + hp - 1) & ~(hp - 1);
    }
    if (e > s && mprotect(g_base + s, e - s, prot) != 0)
        fatal("mprotect(%#x, %#x, %d) failed: %d", a, size, prot, errno);
    set_pages(a, size, prot != PROT_NONE);
}

void mem_init() {
    // 4 GiB of guest space plus a guard so that accesses straddling the top never touch host memory.
    const size_t total = (1ull << 32) + (1 << 20);
    void* p = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve guest address space: %d", errno);
    g_base = static_cast<u8*>(p);
    g_host_page = u32(sysconf(_SC_PAGESIZE));
    g_page_map = static_cast<u8*>(calloc(1u << 20, 1));

    mem_protect(layout::kMiscBase, layout::kMiscSize, PROT_READ | PROT_WRITE);
    g_map_free[layout::kMapBase] = layout::kMapEnd - layout::kMapBase;
    NB_LOGI("guest address space at %p (host page size %u)", g_base, g_host_page);
}

gaddr misc_alloc(u32 size, u32 align) {
    std::lock_guard lk(g_misc_mu);
    gaddr a = (g_misc_next + align - 1) & ~(align - 1);
    if (a + size > layout::kMiscBase + layout::kMiscSize) fatal("misc area exhausted");
    g_misc_next = a + size;
    return a;
}

gaddr map_alloc(u32 size, int prot) {
    size = host_page_round_up(size);
    std::lock_guard lk(g_map_mu);
    for (auto it = g_map_free.begin(); it != g_map_free.end(); ++it) {
        if (it->second < size) continue;
        gaddr a = it->first;
        u32 left = it->second - size;
        g_map_free.erase(it);
        if (left) g_map_free[a + size] = left;
        if (prot != PROT_NONE) mem_protect(a, size, prot);
        return a;
    }
    return 0;
}

bool map_claim(gaddr a, u32 size) {
    size = host_page_round_up(size);
    std::lock_guard lk(g_map_mu);
    auto it = g_map_free.upper_bound(a);
    if (it == g_map_free.begin()) return false;
    --it;
    gaddr fs = it->first;
    u32 fsz = it->second;
    if (u64(a) + size > u64(fs) + fsz) return false;
    g_map_free.erase(it);
    if (a > fs) g_map_free[fs] = a - fs;
    if (u64(a) + size < u64(fs) + fsz) g_map_free[a + size] = u32(u64(fs) + fsz - (u64(a) + size));
    return true;
}

void map_free(gaddr a, u32 size) {
    size = host_page_round_up(size);
    if (a & (g_host_page - 1)) {
        NB_LOGW("map_free of %#x is not host-page aligned; ignored", a);
        return;
    }
    if (a < layout::kMapBase || u64(a) + size > layout::kMapEnd) {
        NB_LOGW("map_free outside map area: %#x+%#x", a, size);
        return;
    }
    // Replace whatever is there (anonymous or file pages) with a fresh inaccessible reservation.
    if (mmap(g_base + a, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
        fatal("map_free: mmap failed %d", errno);
    set_pages(a, size, 0);
    std::lock_guard lk(g_map_mu);
    auto [it, ok] = g_map_free.emplace(a, size);
    if (!ok) {
        NB_LOGW("double map_free at %#x", a);
        return;
    }
    auto next = std::next(it);
    if (next != g_map_free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        g_map_free.erase(next);
    }
    if (it != g_map_free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            g_map_free.erase(it);
        }
    }
}

// ---------------------------------------------------------------- heap

u32 guest_heap_used() { return g_brk - layout::kHeapBase; }

#ifdef NB_HEADLESS
namespace {
std::mutex g_track_mu;
std::map<gaddr, std::pair<u32, u64>>* g_track;  // ptr -> (size, tag)
thread_local int t_track_depth = 0;
}  // namespace
void heap_track(gaddr p, u32 n, u64 tag) {
    if (!p) return;
    std::lock_guard lk(g_track_mu);
    if (!g_track) g_track = new std::map<gaddr, std::pair<u32, u64>>;
    (*g_track)[p] = {n, tag};
}
void heap_untrack(gaddr p) {
    if (!p) return;
    std::lock_guard lk(g_track_mu);
    if (g_track) g_track->erase(p);
}
void heap_report() {
    std::map<u64, std::pair<u64, u64>> by;  // tag -> (bytes, count)
    {
        std::lock_guard lk(g_track_mu);
        if (!g_track) return;
        for (auto& [p, v] : *g_track) {
            by[v.second].first += v.first;
            by[v.second].second++;
        }
    }
    std::vector<std::pair<u64, u64>> v;
    for (auto& [tag, bc] : by) v.push_back({bc.first, tag});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; ++i) {
        u64 tag = v[i].second;
        std::string where = tag & 0x80000000u
                                ? "guest " + symbolize(u32(tag & 0x7fffffffu)) + "  <-  " + symbolize(u32(tag >> 32))
                                : "host " + std::to_string(tag);
        NB_LOGI("live %8llu bytes in %6llu blocks from %s", (unsigned long long)v[i].first,
                (unsigned long long)by[tag].second, where.c_str());
    }
}
#define NB_TAG(p, n) (t_track_depth == 0 ? heap_track(p, n, reinterpret_cast<u64>(__builtin_return_address(0))) : void())
#else
#define NB_TAG(p, n) ((void)0)
#endif

gaddr gmalloc(u32 n) {
    gaddr p = g(nbg_malloc(n));
    NB_TAG(p, n);
    return p;
}
gaddr gcalloc(u32 n, u32 sz) {
    if (sz && n > 0xffffffffu / sz) return 0;
    gaddr p = g(nbg_calloc(n, sz));
    NB_TAG(p, n * sz);
    return p;
}
gaddr grealloc(gaddr p, u32 n) { return g(nbg_realloc(h(p), n)); }
void gfree(gaddr p) {
#ifdef NB_HEADLESS
    heap_untrack(p);
#endif
    if (!p) return;
    if (p < layout::kHeapBase || p >= layout::kHeapEnd) {
        NB_LOGW("free() of non-heap pointer %#x ignored", p);
        return;
    }
    nbg_free(h(p));
}
gaddr gstrdup(const char* s) { return gmemdup(s, u32(strlen(s) + 1)); }
gaddr gmemdup(const void* p, u32 n) {
    gaddr a = gmalloc(n ? n : 1);
    if (a) memcpy(h(a), p, n);
    return a;
}

}  // namespace nb

// MORECORE for dlmalloc: a contiguous arena inside guest space.
extern "C" void* nb_guest_sbrk(ptrdiff_t inc) {
    using namespace nb;
    std::lock_guard lk(g_brk_mu);
    gaddr old = g_brk;
    s64 nbrk = s64(g_brk) + inc;
    if (nbrk < layout::kHeapBase || nbrk > layout::kHeapEnd) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(~uintptr_t(0));
    }
    g_brk = gaddr(nbrk);
    gaddr want = host_page_round_up(g_brk);
    if (want > g_brk_committed) {
        mem_protect(g_brk_committed, want - g_brk_committed, PROT_READ | PROT_WRITE);
        g_brk_committed = want;
    } else if (want < g_brk_committed) {
        madvise(g_base + want, g_brk_committed - want, MADV_DONTNEED);
        mem_protect(want, g_brk_committed - want, PROT_NONE);
        g_brk_committed = want;
    }
    return g_base + old;
}

extern "C" void nb_guest_heap_abort() { nb::fatal("guest heap corrupted (dlmalloc abort)"); }

extern "C" unsigned nb_guest_heap_used_kb() { return nb::guest_heap_used() >> 10; }
