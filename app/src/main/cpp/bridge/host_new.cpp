// Route every C++ allocation made inside libgl2jni.so (dynarmic, libc++, the bridge) to the private
// heap in host_heap.c. libc++ is linked statically and these definitions are hidden, so nothing
// outside this library is affected, and malloc()/free() keep using the system allocator.
#include <cstddef>
#include <new>

extern "C" {
void* nbh_malloc(size_t);
void nbh_free(void*);
void* nbh_memalign(size_t, size_t);
}

namespace {

void* alloc(size_t n) {
    for (;;) {
        if (void* p = nbh_malloc(n ? n : 1)) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}

void* alloc_aligned(size_t n, std::align_val_t a) {
    for (;;) {
        if (void* p = nbh_memalign(static_cast<size_t>(a), n ? n : 1)) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}

}  // namespace

void* operator new(size_t n) { return alloc(n); }
void* operator new[](size_t n) { return alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return nbh_malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return nbh_malloc(n ? n : 1); }
void* operator new(size_t n, std::align_val_t a) { return alloc_aligned(n, a); }
void* operator new[](size_t n, std::align_val_t a) { return alloc_aligned(n, a); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return nbh_memalign(static_cast<size_t>(a), n ? n : 1);
}
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return nbh_memalign(static_cast<size_t>(a), n ? n : 1);
}

void operator delete(void* p) noexcept { nbh_free(p); }
void operator delete[](void* p) noexcept { nbh_free(p); }
void operator delete(void* p, size_t) noexcept { nbh_free(p); }
void operator delete[](void* p, size_t) noexcept { nbh_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { nbh_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { nbh_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { nbh_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { nbh_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { nbh_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { nbh_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { nbh_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { nbh_free(p); }
