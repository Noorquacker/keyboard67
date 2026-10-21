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
#define dlrealloc_in_place nbg_realloc_in_place
#define dlmemalign nbg_memalign
#define dlposix_memalign nbg_posix_memalign
#define dlvalloc nbg_valloc
#define dlpvalloc nbg_pvalloc
#define dlmallopt nbg_mallopt
#define dlmalloc_trim nbg_malloc_trim
#define dlmalloc_stats nbg_malloc_stats
#define dlmalloc_usable_size nbg_malloc_usable_size
#define dlmalloc_footprint nbg_malloc_footprint
#define dlmalloc_max_footprint nbg_malloc_max_footprint
#define dlmalloc_footprint_limit nbg_malloc_footprint_limit
#define dlmalloc_set_footprint_limit nbg_malloc_set_footprint_limit
#define dlmalloc_inspect_all nbg_malloc_inspect_all
#define dlmallinfo nbg_mallinfo
#define dlindependent_calloc nbg_independent_calloc
#define dlindependent_comalloc nbg_independent_comalloc
#define dlbulk_free nbg_bulk_free

#include "../third_party/dlmalloc/malloc.c"
