// dlmalloc configured to allocate exclusively from the guest's sbrk arena, so that every block
// handed to the guest lives inside its 32-bit address space.
#include <stddef.h>

void* nb_guest_sbrk(ptrdiff_t inc);
void nb_guest_heap_abort(void);

#define USE_DL_PREFIX 1
#define DLMALLOC_EXPORT __attribute__((visibility("hidden")))
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE nb_guest_sbrk
#define MORECORE_CONTIGUOUS 1
#define USE_LOCKS 1
#define DEFAULT_GRANULARITY (1024 * 1024)
#define ABORT nb_guest_heap_abort()
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1

#define dlmalloc nbg_malloc
#define dlfree nbg_free
#define dlcalloc nbg_calloc
#define dlrealloc nbg_realloc

#include "../third_party/dlmalloc/malloc.c"
