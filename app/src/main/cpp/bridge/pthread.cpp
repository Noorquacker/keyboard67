// pthread / semaphore imports. Guest threads are real host threads with their own guest stack
// and JIT instance. Guest synchronisation objects are 4-byte words (old bionic ABI); the first
// use swaps the static initializer for a handle to a host object.
#include <pthread.h>
#include <semaphore.h>

#include <cerrno>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "cpu.h"
#include "nb.h"

namespace nb {
namespace {

constexpr u32 kHandleBit = 0x80000000u;
constexpr u32 kMutexRecursiveInit = 0x4000, kMutexErrorcheckInit = 0x8000;

template <class T>
struct Pool {
    std::mutex mu;
    std::deque<T> items;
    std::vector<u32> free_list;

    u32 add(std::function<void(T&)> init) {
        std::lock_guard lk(mu);
        u32 idx;
        if (!free_list.empty()) {
            idx = free_list.back();
            free_list.pop_back();
        } else {
            idx = u32(items.size());
            items.emplace_back();
        }
        init(items[idx]);
        return idx;
    }
    T& get(u32 idx) {
        std::lock_guard lk(mu);
        return items[idx];
    }
    void release(u32 idx) {
        std::lock_guard lk(mu);
        free_list.push_back(idx);
    }
};

Pool<pthread_mutex_t> g_mutexes;
Pool<pthread_cond_t> g_conds;
Pool<sem_t> g_sems;

pthread_mutex_t* new_mutex_handle(gaddr gm, u32 type_word, bool force) {
    int type = type_word == kMutexRecursiveInit   ? PTHREAD_MUTEX_RECURSIVE
               : type_word == kMutexErrorcheckInit ? PTHREAD_MUTEX_ERRORCHECK
                                                   : PTHREAD_MUTEX_NORMAL;
    u32 idx = g_mutexes.add([type](pthread_mutex_t& m) {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        pthread_mutexattr_settype(&at, type);
        pthread_mutex_init(&m, &at);
        pthread_mutexattr_destroy(&at);
    });
    u32 handle = kHandleBit | idx;
    if (force) {
        wr<u32>(gm, handle);
        return &g_mutexes.get(idx);
    }
    u32 expected = type_word;
    if (!__atomic_compare_exchange_n(hp<u32>(gm), &expected, handle, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        // another thread won the race
        pthread_mutex_destroy(&g_mutexes.get(idx));
        g_mutexes.release(idx);
        return &g_mutexes.get(expected & ~kHandleBit);
    }
    return &g_mutexes.get(idx);
}

pthread_mutex_t* mutex_of(gaddr gm) {
    u32 w = __atomic_load_n(hp<u32>(gm), __ATOMIC_ACQUIRE);
    if (w & kHandleBit) return &g_mutexes.get(w & ~kHandleBit);
    return new_mutex_handle(gm, w, false);
}

pthread_cond_t* cond_of(gaddr gc) {
    u32 w = __atomic_load_n(hp<u32>(gc), __ATOMIC_ACQUIRE);
    if (w & kHandleBit) return &g_conds.get(w & ~kHandleBit);
    u32 idx = g_conds.add([](pthread_cond_t& c) { pthread_cond_init(&c, nullptr); });
    u32 expected = w;
    if (!__atomic_compare_exchange_n(hp<u32>(gc), &expected, kHandleBit | idx, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        pthread_cond_destroy(&g_conds.get(idx));
        g_conds.release(idx);
        return &g_conds.get(expected & ~kHandleBit);
    }
    return &g_conds.get(idx);
}

// ------------------------------------------------------------------ mutex / cond

int x_mutexattr_init(u32 a) {
    wr<u32>(a, 0);
    return 0;
}
int x_mutexattr_destroy(u32) { return 0; }
int x_mutexattr_settype(u32 a, int type) {
    if (type < 0 || type > 2) return EINVAL;
    wr<u32>(a, u32(type));
    return 0;
}
int x_mutex_init(u32 m, u32 attr) {
    u32 type = attr ? rd<u32>(attr) : 0;
    u32 word = type == 1 ? kMutexRecursiveInit : type == 2 ? kMutexErrorcheckInit : 0;
    new_mutex_handle(m, word, true);
    return 0;
}
int x_mutex_destroy(u32 m) {
    u32 w = rd<u32>(m);
    if (w & kHandleBit) {
        pthread_mutex_destroy(&g_mutexes.get(w & ~kHandleBit));
        g_mutexes.release(w & ~kHandleBit);
    }
    wr<u32>(m, 0xdead0000u);  // neither a handle nor an initializer
    return 0;
}
int x_mutex_lock(u32 m) { return pthread_mutex_lock(mutex_of(m)); }
int x_mutex_trylock(u32 m) { return pthread_mutex_trylock(mutex_of(m)); }
int x_mutex_unlock(u32 m) { return pthread_mutex_unlock(mutex_of(m)); }

int x_cond_wait(u32 c, u32 m) { return pthread_cond_wait(cond_of(c), mutex_of(m)); }
int x_cond_broadcast(u32 c) { return pthread_cond_broadcast(cond_of(c)); }

// ------------------------------------------------------------------ semaphores

int x_sem_init(u32 s, int, u32 value) {
    u32 idx = g_sems.add([value](sem_t& sem) { sem_init(&sem, 0, value); });
    wr<u32>(s, kHandleBit | idx);
    return 0;
}
sem_t* sem_of(u32 s) {
    u32 w = rd<u32>(s);
    if (!(w & kHandleBit)) fatal("guest used an uninitialised semaphore at %#x", s);
    return &g_sems.get(w & ~kHandleBit);
}
int x_sem_destroy(u32 s) {
    u32 w = rd<u32>(s);
    if (w & kHandleBit) {
        sem_destroy(&g_sems.get(w & ~kHandleBit));
        g_sems.release(w & ~kHandleBit);
        wr<u32>(s, 0);
    }
    return 0;
}
int x_sem_post(u32 s) { return sem_post(sem_of(s)) ? (errno_from_host(), -1) : 0; }
int x_sem_wait(u32 s) {
    int r;
    while ((r = sem_wait(sem_of(s))) != 0 && errno == EINTR) {}
    if (r) errno_from_host();
    return r;
}

// ------------------------------------------------------------------ threads

struct ThreadRec {
    pthread_t host{};
    u32 tid = 0;
    gaddr fn = 0, arg = 0;
    std::atomic<bool> done{false};
    u32 retval = 0;
};
std::mutex g_threads_mu;
std::unordered_map<u32, std::shared_ptr<ThreadRec>> g_threads;

void* thread_main(void* p) {
    auto rec = *static_cast<std::shared_ptr<ThreadRec>*>(p);
    delete static_cast<std::shared_ptr<ThreadRec>*>(p);
    GuestThread& t = cur_thread();
    t.tid = rec->tid;
    u32 r = u32(call_guest(rec->fn, {ga(rec->arg)}));
    rec->retval = t.exiting ? t.exit_value : r;
    rec->done = true;
    return nullptr;
}

std::atomic<u32> g_guest_tids{0x1000};

int x_pthread_create(u32 out, u32 attr, u32 fn, u32 arg) {
    (void)attr;
    auto rec = std::make_shared<ThreadRec>();
    rec->tid = g_guest_tids++;
    rec->fn = fn;
    rec->arg = arg;
    {
        std::lock_guard lk(g_threads_mu);
        g_threads[rec->tid] = rec;
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 2u << 20);
    int r = pthread_create(&rec->host, &at, thread_main, new std::shared_ptr<ThreadRec>(rec));
    pthread_attr_destroy(&at);
    if (r) {
        std::lock_guard lk(g_threads_mu);
        g_threads.erase(rec->tid);
        return r;
    }
    if (out) wr<u32>(out, rec->tid);
    NB_LOGI("guest thread %u started at %s", rec->tid, symbolize(fn).c_str());
    return 0;
}

int x_pthread_join(u32 tid, u32 ret) {
    std::shared_ptr<ThreadRec> rec;
    {
        std::lock_guard lk(g_threads_mu);
        auto it = g_threads.find(tid);
        if (it == g_threads.end()) return ESRCH;
        rec = it->second;
    }
    int r = pthread_join(rec->host, nullptr);
    if (r) return r;
    if (ret) wr<u32>(ret, rec->retval);
    std::lock_guard lk(g_threads_mu);
    g_threads.erase(tid);
    return 0;
}

void x_pthread_exit(Ctx& c) {
    GuestThread& t = c.thread();
    t.exit_value = c.a32();
    t.exiting = true;
    if (t.depth > 0) NB_LOGW("pthread_exit from a nested guest call; unwinding all levels");
    for (auto& cpu : t.cpus) cpu->jit->HaltExecution();
}

u32 x_pthread_self() {
    GuestThread& t = cur_thread();
    return t.tid;
}

int x_pthread_kill(u32 tid, int sig) {
    if (sig != 0) NB_LOGW("pthread_kill(%u, %d) ignored", tid, sig);
    if (tid == cur_thread().tid) return 0;
    std::lock_guard lk(g_threads_mu);
    auto it = g_threads.find(tid);
    if (it == g_threads.end() || it->second->done) return ESRCH;
    return 0;
}

// ------------------------------------------------------------------ keys / once

std::mutex g_keys_mu;
std::vector<bool> g_keys_used;

int x_key_create(u32 out, u32 dtor) {
    (void)dtor;  // destructors are not run; guest threads are long-lived
    std::lock_guard lk(g_keys_mu);
    for (size_t i = 0; i < g_keys_used.size(); ++i) {
        if (!g_keys_used[i]) {
            g_keys_used[i] = true;
            wr<u32>(out, u32(i));
            return 0;
        }
    }
    if (g_keys_used.size() >= 1024) return EAGAIN;
    g_keys_used.push_back(true);
    wr<u32>(out, u32(g_keys_used.size() - 1));
    return 0;
}
int x_key_delete(u32 key) {
    std::lock_guard lk(g_keys_mu);
    if (key < g_keys_used.size()) g_keys_used[key] = false;
    return 0;
}
u32 x_getspecific(u32 key) {
    auto& v = cur_thread().specific;
    return key < v.size() ? v[key] : 0;
}
int x_setspecific(u32 key, u32 val) {
    auto& v = cur_thread().specific;
    if (key >= v.size()) v.resize(key + 1, 0);
    v[key] = val;
    return 0;
}

std::recursive_mutex g_once_mu;
int x_pthread_once(u32 once, u32 fn) {
    if (__atomic_load_n(hp<u32>(once), __ATOMIC_ACQUIRE) == 2) return 0;
    std::lock_guard lk(g_once_mu);
    if (rd<u32>(once) != 0) return 0;
    wr<u32>(once, 1);
    call_guest(fn, {});
    __atomic_store_n(hp<u32>(once), 2u, __ATOMIC_RELEASE);
    return 0;
}

}  // namespace

void register_pthread() {
    NB_REG("pthread_mutexattr_init", x_mutexattr_init);
    NB_REG("pthread_mutexattr_destroy", x_mutexattr_destroy);
    NB_REG("pthread_mutexattr_settype", x_mutexattr_settype);
    NB_REG("pthread_mutex_init", x_mutex_init);
    NB_REG("pthread_mutex_destroy", x_mutex_destroy);
    NB_REG("pthread_mutex_lock", x_mutex_lock);
    NB_REG("pthread_mutex_trylock", x_mutex_trylock);
    NB_REG("pthread_mutex_unlock", x_mutex_unlock);
    NB_REG("pthread_cond_wait", x_cond_wait);
    NB_REG("pthread_cond_broadcast", x_cond_broadcast);
    NB_REG("sem_init", x_sem_init);
    NB_REG("sem_destroy", x_sem_destroy);
    NB_REG("sem_post", x_sem_post);
    NB_REG("sem_wait", x_sem_wait);
    NB_REG("pthread_create", x_pthread_create);
    NB_REG("pthread_join", x_pthread_join);
    reg("pthread_exit", x_pthread_exit);
    NB_REG("pthread_self", x_pthread_self);
    NB_REG("pthread_kill", x_pthread_kill);
    NB_REG("pthread_key_create", x_key_create);
    NB_REG("pthread_key_delete", x_key_delete);
    NB_REG("pthread_getspecific", x_getspecific);
    NB_REG("pthread_setspecific", x_setspecific);
    NB_REG("pthread_once", x_pthread_once);
}

}  // namespace nb
