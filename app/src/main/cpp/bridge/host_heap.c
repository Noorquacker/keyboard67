// A private dlmalloc instance for the bridge's own C++ objects (see host_new.cpp). The JIT makes
// enormous numbers of small, short-lived allocations while compiling, and the platform allocator
// (scudo, with its hardening) was ~1 s of a cold start. This heap uses ordinary host mmap and is
// entirely separate from the guest heap in guest_heap.c.
#include <stddef.h>

#define USE_DL_PREFIX 1
#define DLMALLOC_EXPORT __attribute__((visibility("hidden")))
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define USE_LOCKS 1
#define DEFAULT_GRANULARITY (2 * 1024 * 1024)
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1


#define dlmalloc nbh_malloc
#define dlfree nbh_free
#define dlcalloc nbh_calloc
#define dlrealloc nbh_realloc
#define dlrealloc_in_place nbh_realloc_in_place
#define dlmemalign nbh_memalign
#define dlposix_memalign nbh_posix_memalign
#define dlvalloc nbh_valloc
#define dlpvalloc nbh_pvalloc
#define dlmallopt nbh_mallopt
#define dlmalloc_trim nbh_malloc_trim
#define dlmalloc_stats nbh_malloc_stats
#define dlmalloc_usable_size nbh_malloc_usable_size
#define dlmalloc_footprint nbh_malloc_footprint
#define dlmalloc_max_footprint nbh_malloc_max_footprint
#define dlmalloc_footprint_limit nbh_malloc_footprint_limit
#define dlmalloc_set_footprint_limit nbh_malloc_set_footprint_limit
#define dlmalloc_inspect_all nbh_malloc_inspect_all
#define dlmallinfo nbh_mallinfo
#define dlindependent_calloc nbh_independent_calloc
#define dlindependent_comalloc nbh_independent_comalloc
#define dlbulk_free nbh_bulk_free

#include "../third_party/dlmalloc/malloc.c"
