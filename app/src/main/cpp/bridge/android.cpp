// liblog and libandroid (AAsset) imports. On non-Android hosts (desktop test harness) the log goes
// to stderr and assets are read from the directory named by $NIN_ASSETS.
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "jni_bridge.h"
#include "nb.h"

// Headless test builds use the desktop code paths even when targeting Android.
#if defined(__ANDROID__) && !defined(NB_HEADLESS)
#define NB_ANDROID_RUNTIME 1
#endif

#ifdef NB_ANDROID_RUNTIME
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace nb {

void log_write(int prio, const char* tag, const char* msg) {
#ifdef NB_ANDROID_RUNTIME
    __android_log_write(prio, tag, msg);
#else
    static const char kLetters[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: %s\n", prio >= 0 && prio < 8 ? kLetters[prio] : '?', tag ? tag : "", msg);
#endif
}

namespace {

void x_log_print(Ctx& c) {
    int prio = int(c.a32());
    const char* tag = hs(c.a32());
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    log_write(prio, tag ? tag : "nin", s.c_str());
    c.ret(1);
}

int x_log_write(int prio, const char* tag, const char* text) {
    log_write(prio, tag ? tag : "nin", text ? text : "");
    return 1;
}

// ------------------------------------------------------------------ assets

struct Asset {
#ifdef NB_ANDROID_RUNTIME
    AAsset* a = nullptr;
#else
    std::string data;
#endif
    gaddr buf = 0;
};

std::mutex g_mu;
std::unordered_map<u32, void*> g_managers;  // guest handle -> AAssetManager*
std::unordered_map<u32, Asset*> g_assets;
u32 g_next_handle = 0x10;

u32 x_mgr_from_java(u32 /*env*/, u32 jam) {
#ifdef NB_ANDROID_RUNTIME
    // The JNIEnv the guest passed is our own; use the host env of this thread instead.
    AAssetManager* m = AAssetManager_fromJava(jni_env(), jni_obj(jam));
    if (!m) return 0;
#else
    (void)jam;
    void* m = reinterpret_cast<void*>(1);
#endif
    std::lock_guard lk(g_mu);
    for (auto& [hnd, ptr] : g_managers)
        if (ptr == m) return hnd;
    u32 hnd = g_next_handle++ << 4;
    g_managers[hnd] = m;
    return hnd;
}

u32 x_asset_open(u32 mgr, const char* name, int mode) {
    void* m;
    {
        std::lock_guard lk(g_mu);
        auto it = g_managers.find(mgr);
        if (it == g_managers.end()) return 0;
        m = it->second;
    }
    auto* as = new Asset;
#ifdef NB_ANDROID_RUNTIME
    as->a = AAssetManager_open(static_cast<AAssetManager*>(m), name, mode);
    if (!as->a) {
        delete as;
        return 0;
    }
#else
    (void)m;
    (void)mode;
    const char* root = getenv("NIN_ASSETS");
    std::ifstream f(std::string(root ? root : "assets") + "/" + name, std::ios::binary);
    if (!f) {
        delete as;
        return 0;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    as->data = ss.str();
#endif
    std::lock_guard lk(g_mu);
    u32 hnd = g_next_handle++ << 4;
    g_assets[hnd] = as;
    return hnd;
}

Asset* asset(u32 hnd) {
    std::lock_guard lk(g_mu);
    auto it = g_assets.find(hnd);
    if (it == g_assets.end()) fatal("guest used an invalid AAsset handle %#x", hnd);
    return it->second;
}

s32 x_asset_length(u32 hnd) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s32(AAsset_getLength(as->a));
#else
    return s32(as->data.size());
#endif
}

u32 x_asset_buffer(u32 hnd) {
    Asset* as = asset(hnd);
    if (as->buf) return as->buf;
#ifdef NB_ANDROID_RUNTIME
    const void* p = AAsset_getBuffer(as->a);
    if (!p) return 0;
    as->buf = gmemdup(p, u32(AAsset_getLength(as->a)));
#else
    as->buf = gmemdup(as->data.data(), u32(as->data.size()));
#endif
    return as->buf;
}

void x_asset_close(u32 hnd) {
    Asset* as;
    {
        std::lock_guard lk(g_mu);
        auto it = g_assets.find(hnd);
        if (it == g_assets.end()) return;
        as = it->second;
        g_assets.erase(it);
    }
#ifdef NB_ANDROID_RUNTIME
    AAsset_close(as->a);
#endif
    gfree(as->buf);
    delete as;
}

}  // namespace

void register_android() {
    reg("__android_log_print", x_log_print);
    NB_REG("__android_log_write", x_log_write);
    NB_REG("AAssetManager_fromJava", x_mgr_from_java);
    NB_REG("AAssetManager_open", x_asset_open);
    NB_REG("AAsset_getLength", x_asset_length);
    NB_REG("AAsset_getBuffer", x_asset_buffer);
    NB_REG("AAsset_close", x_asset_close);
}

}  // namespace nb
