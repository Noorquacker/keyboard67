// JNI for the guest. The guest receives its own JNIEnv / JavaVM whose function tables point at
// stubs; object references are 32-bit handles into a table of host references, and calls are
// marshalled using the parsed method signature.
#include "jni_bridge.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "cpu.h"

namespace nb {

#ifdef __ANDROID__
using HostNI = JNINativeInterface;
using HostII = JNIInvokeInterface;
#else
using HostNI = JNINativeInterface_;
using HostII = JNIInvokeInterface_;
#endif

#define JNI_INDEX(fn) u32(offsetof(HostNI, fn) / sizeof(void*))
#define JII_INDEX(fn) u32(offsetof(HostII, fn) / sizeof(void*))

namespace {

JavaVM* g_vm = nullptr;
gaddr g_env_table = 0, g_guest_env = 0, g_vm_table = 0, g_guest_vm = 0;
thread_local JNIEnv* t_env = nullptr;

JNIEnv* env() {
    if (t_env) return t_env;
    JNIEnv* e = nullptr;
    if (!g_vm || g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK || !e)
        fatal("guest used JNI on a thread that is not attached to the JVM");
    t_env = e;
    return e;
}

// ------------------------------------------------------------------ reference table

constexpr u32 kMaxRefs = 1u << 20;
constexpr u32 kGlobalFrame = 0, kWeakFrame = 1;

struct Slot {
    jobject obj;
    u32 frame;
    u32 gen;
};
Slot* g_slots = nullptr;
std::mutex g_ref_mu;
std::vector<u32> g_ref_free;
u32 g_ref_next = 1;
std::atomic<u32> g_frame_ids{2};

struct Frame {
    u32 id;
    std::vector<std::pair<u32, u32>> slots;  // (index, gen)
};
thread_local std::vector<Frame> t_frames;

u32 alloc_slot(jobject o, u32 frame, u32* gen_out) {
    std::lock_guard lk(g_ref_mu);
    u32 idx;
    if (!g_ref_free.empty()) {
        idx = g_ref_free.back();
        g_ref_free.pop_back();
    } else {
        if (g_ref_next >= kMaxRefs) fatal("guest JNI reference table overflow");
        idx = g_ref_next++;
    }
    Slot& s = g_slots[idx];
    s.obj = o;
    s.frame = frame;
    *gen_out = ++s.gen;
    return idx;
}

void free_slot(u32 idx) {
    std::lock_guard lk(g_ref_mu);
    g_slots[idx].obj = nullptr;
    g_slots[idx].frame = ~0u;
    g_ref_free.push_back(idx);
}

Frame& top_frame() {
    if (t_frames.empty()) t_frames.push_back({g_frame_ids++, {}});  // natively attached thread
    return t_frames.back();
}

void pop_frame() {
    Frame f = std::move(t_frames.back());
    t_frames.pop_back();
    std::lock_guard lk(g_ref_mu);
    for (auto [idx, gen] : f.slots) {
        Slot& s = g_slots[idx];
        if (s.frame == f.id && s.gen == gen) {
            s.obj = nullptr;
            s.frame = ~0u;
            g_ref_free.push_back(idx);
        }
    }
}

u32 local(jobject o) {
    if (!o) return 0;
    Frame& f = top_frame();
    u32 gen;
    u32 idx = alloc_slot(o, f.id, &gen);
    f.slots.push_back({idx, gen});
    return idx << 2;
}

u32 global(jobject o, u32 kind) {
    if (!o) return 0;
    u32 gen;
    return alloc_slot(o, kind, &gen) << 2;
}

jobject obj(u32 hnd) {
    if (!hnd) return nullptr;
    u32 idx = hnd >> 2;
    if ((hnd & 3) || idx >= kMaxRefs) fatal("guest passed an invalid JNI reference %#x", hnd);
    return g_slots[idx].obj;
}

// ------------------------------------------------------------------ method / field ids

struct MethodInfo {
    jmethodID id;
    std::string args;  // one type char per argument ('L' for any reference)
    char ret;
};
struct FieldInfo {
    jfieldID id;
    char type;
};
std::mutex g_id_mu;
std::deque<MethodInfo> g_methods;
std::unordered_map<jmethodID, u32> g_method_handles;
std::deque<FieldInfo> g_fields;
std::unordered_map<jfieldID, u32> g_field_handles;

char parse_type(const char*& p) {
    char t = *p;
    if (t == '[') {
        while (*p == '[') ++p;
        if (*p == 'L') {
            while (*p && *p != ';') ++p;
        }
        if (*p) ++p;
        return 'L';
    }
    if (t == 'L') {
        while (*p && *p != ';') ++p;
        if (*p) ++p;
        return 'L';
    }
    ++p;
    return t;
}

u32 method_handle(jmethodID id, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(g_id_mu);
    auto it = g_method_handles.find(id);
    if (it != g_method_handles.end()) return it->second;
    MethodInfo mi{id, {}, 'V'};
    const char* p = sig;
    if (*p == '(') ++p;
    while (*p && *p != ')') mi.args += parse_type(p);
    if (*p == ')') ++p;
    mi.ret = parse_type(p);
    g_methods.push_back(std::move(mi));
    u32 hnd = u32(g_methods.size()) << 4;
    g_method_handles[id] = hnd;
    return hnd;
}

const MethodInfo& method(u32 hnd) {
    std::lock_guard lk(g_id_mu);
    u32 i = hnd >> 4;
    if (!i || i > g_methods.size()) fatal("guest passed an invalid jmethodID %#x", hnd);
    return g_methods[i - 1];
}

u32 field_handle(jfieldID id, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(g_id_mu);
    auto it = g_field_handles.find(id);
    if (it != g_field_handles.end()) return it->second;
    const char* p = sig;
    g_fields.push_back({id, parse_type(p)});
    u32 hnd = u32(g_fields.size()) << 4 | 8;
    g_field_handles[id] = hnd;
    return hnd;
}

jfieldID field(u32 hnd) {
    std::lock_guard lk(g_id_mu);
    u32 i = hnd >> 4;
    if (!i || i > g_fields.size()) fatal("guest passed an invalid jfieldID %#x", hnd);
    return g_fields[i - 1].id;
}

// ------------------------------------------------------------------ argument helpers

// Arguments passed through C varargs (default promotions apply).
jvalue va_value(char t, ArgSrc& a) {
    jvalue v{};
    switch (t) {
    case 'Z': v.z = jboolean(a.next32()); break;
    case 'B': v.b = jbyte(a.next32()); break;
    case 'C': v.c = jchar(a.next32()); break;
    case 'S': v.s = jshort(a.next32()); break;
    case 'I': v.i = jint(a.next32()); break;
    case 'J': v.j = jlong(a.next64()); break;
    case 'F': {
        u64 b = a.next64();
        double d;
        memcpy(&d, &b, 8);
        v.f = float(d);
        break;
    }
    case 'D': {
        u64 b = a.next64();
        memcpy(&v.d, &b, 8);
        break;
    }
    default: v.l = obj(a.next32()); break;
    }
    return v;
}

// Element of a guest jvalue[] (8 bytes each).
jvalue array_value(char t, gaddr p) {
    jvalue v{};
    switch (t) {
    case 'Z': v.z = rd<u8>(p); break;
    case 'B': v.b = rd<s8>(p); break;
    case 'C': v.c = rd<u16>(p); break;
    case 'S': v.s = rd<s16>(p); break;
    case 'I': v.i = rd<s32>(p); break;
    case 'J': v.j = rd<s64>(p); break;
    case 'F': v.f = rd<float>(p); break;
    case 'D': v.d = rd<double>(p); break;
    default: v.l = obj(rd<u32>(p)); break;
    }
    return v;
}

void put_value(Ctx& c, char t, jvalue v) {
    switch (t) {
    case 'V': break;
    case 'Z': c.ret(v.z); break;
    case 'B': c.ret(u32(s32(v.b))); break;
    case 'C': c.ret(v.c); break;
    case 'S': c.ret(u32(s32(v.s))); break;
    case 'I': c.ret(u32(v.i)); break;
    case 'J': c.ret64(u64(v.j)); break;
    case 'F': put_ret<float>(c, v.f); break;
    case 'D': put_ret<double>(c, v.d); break;
    default: c.ret(local(v.l)); break;
    }
}

// Non-variadic scalar parameter of type t (e.g. the value of SetIntField).
jvalue param_value(char t, Ctx& c) {
    jvalue v{};
    switch (t) {
    case 'J': v.j = jlong(c.a64()); break;
    case 'D': v.d = fetch_arg<double>(c); break;
    case 'F': v.f = fetch_arg<float>(c); break;
    default: {
        CtxArgs a(c);
        v = va_value(t, a);
    }
    }
    return v;
}

// ------------------------------------------------------------------ calls

enum Kind { kVirtual, kNonvirtual, kStatic };
enum Form { kDots, kV, kA };

jvalue host_call(Kind k, char ret, jobject o, jclass cls, jmethodID m, const jvalue* a) {
    JNIEnv* e = env();
    jvalue r{};
#define NB_CALL(T, field)                                     \
    if (k == kVirtual)                                        \
        r.field = e->Call##T##MethodA(o, m, a);               \
    else if (k == kNonvirtual)                                \
        r.field = e->CallNonvirtual##T##MethodA(o, cls, m, a); \
    else                                                      \
        r.field = e->CallStatic##T##MethodA(cls, m, a);
    switch (ret) {
    case 'Z': NB_CALL(Boolean, z) break;
    case 'B': NB_CALL(Byte, b) break;
    case 'C': NB_CALL(Char, c) break;
    case 'S': NB_CALL(Short, s) break;
    case 'I': NB_CALL(Int, i) break;
    case 'J': NB_CALL(Long, j) break;
    case 'F': NB_CALL(Float, f) break;
    case 'D': NB_CALL(Double, d) break;
    case 'V':
        if (k == kVirtual)
            e->CallVoidMethodA(o, m, a);
        else if (k == kNonvirtual)
            e->CallNonvirtualVoidMethodA(o, cls, m, a);
        else
            e->CallStaticVoidMethodA(cls, m, a);
        break;
    default: NB_CALL(Object, l) break;
    }
#undef NB_CALL
    return r;
}

std::vector<jvalue> read_args(const MethodInfo& mi, Form f, Ctx& c) {
    std::vector<jvalue> v(mi.args.size());
    if (f == kDots) {
        CtxArgs a(c);
        for (size_t i = 0; i < v.size(); ++i) v[i] = va_value(mi.args[i], a);
    } else if (f == kV) {
        VaArgs a(c.a32());
        for (size_t i = 0; i < v.size(); ++i) v[i] = va_value(mi.args[i], a);
    } else {
        gaddr p = c.a32();
        for (size_t i = 0; i < v.size(); ++i) v[i] = array_value(mi.args[i], p + u32(i) * 8);
    }
    return v;
}

template <int K, int F, char T>
void jni_call(Ctx& c) {
    c.a32();  // env
    jobject o = nullptr;
    jclass cls = nullptr;
    if (K == kStatic) {
        cls = static_cast<jclass>(obj(c.a32()));
    } else {
        o = obj(c.a32());
        if (K == kNonvirtual) cls = static_cast<jclass>(obj(c.a32()));
    }
    const MethodInfo& mi = method(c.a32());
    std::vector<jvalue> args = read_args(mi, Form(F), c);
    jvalue r = host_call(Kind(K), T, o, cls, mi.id, args.data());
    put_value(c, T, r);
}

template <int F>
void jni_new_object(Ctx& c) {
    c.a32();
    jclass cls = static_cast<jclass>(obj(c.a32()));
    const MethodInfo& mi = method(c.a32());
    std::vector<jvalue> args = read_args(mi, Form(F), c);
    c.ret(local(env()->NewObjectA(cls, mi.id, args.data())));
}

// ------------------------------------------------------------------ fields

template <char T, bool Static>
void jni_get_field(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    jfieldID f = field(c.a32());
    JNIEnv* e = env();
    jvalue v{};
#define NB_GET(Ty, fld) \
    v.fld = Static ? e->GetStatic##Ty##Field(static_cast<jclass>(o), f) : e->Get##Ty##Field(o, f);
    switch (T) {
    case 'Z': NB_GET(Boolean, z) break;
    case 'B': NB_GET(Byte, b) break;
    case 'C': NB_GET(Char, c) break;
    case 'S': NB_GET(Short, s) break;
    case 'I': NB_GET(Int, i) break;
    case 'J': NB_GET(Long, j) break;
    case 'F': NB_GET(Float, f) break;
    case 'D': NB_GET(Double, d) break;
    default: NB_GET(Object, l) break;
    }
#undef NB_GET
    put_value(c, T, v);
}

template <char T, bool Static>
void jni_set_field(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    jfieldID f = field(c.a32());
    jvalue v = param_value(T, c);
    JNIEnv* e = env();
#define NB_SET(Ty, fld)                                                     \
    if (Static)                                                             \
        e->SetStatic##Ty##Field(static_cast<jclass>(o), f, v.fld);          \
    else                                                                    \
        e->Set##Ty##Field(o, f, v.fld);
    switch (T) {
    case 'Z': NB_SET(Boolean, z) break;
    case 'B': NB_SET(Byte, b) break;
    case 'C': NB_SET(Char, c) break;
    case 'S': NB_SET(Short, s) break;
    case 'I': NB_SET(Int, i) break;
    case 'J': NB_SET(Long, j) break;
    case 'F': NB_SET(Float, f) break;
    case 'D': NB_SET(Double, d) break;
    default: NB_SET(Object, l) break;
    }
#undef NB_SET
}

// ------------------------------------------------------------------ primitive arrays

constexpr u32 elem_size(char t) {
    return t == 'J' || t == 'D' ? 8 : t == 'I' || t == 'F' ? 4 : t == 'C' || t == 'S' ? 2 : 1;
}

void array_region(JNIEnv* e, char t, bool set, jarray a, jsize start, jsize len, void* buf) {
#define NB_REGION(Ty, jt)                                                                          \
    if (set)                                                                                       \
        e->Set##Ty##ArrayRegion(static_cast<jt##Array>(a), start, len, static_cast<const jt*>(buf)); \
    else                                                                                           \
        e->Get##Ty##ArrayRegion(static_cast<jt##Array>(a), start, len, static_cast<jt*>(buf));
    switch (t) {
    case 'Z': NB_REGION(Boolean, jboolean) break;
    case 'B': NB_REGION(Byte, jbyte) break;
    case 'C': NB_REGION(Char, jchar) break;
    case 'S': NB_REGION(Short, jshort) break;
    case 'I': NB_REGION(Int, jint) break;
    case 'J': NB_REGION(Long, jlong) break;
    case 'F': NB_REGION(Float, jfloat) break;
    case 'D': NB_REGION(Double, jdouble) break;
    }
#undef NB_REGION
}

template <char T>
void jni_new_array(Ctx& c) {
    c.a32();
    jsize n = jsize(c.a32());
    JNIEnv* e = env();
    jarray a = nullptr;
    switch (T) {
    case 'Z': a = e->NewBooleanArray(n); break;
    case 'B': a = e->NewByteArray(n); break;
    case 'C': a = e->NewCharArray(n); break;
    case 'S': a = e->NewShortArray(n); break;
    case 'I': a = e->NewIntArray(n); break;
    case 'J': a = e->NewLongArray(n); break;
    case 'F': a = e->NewFloatArray(n); break;
    case 'D': a = e->NewDoubleArray(n); break;
    }
    c.ret(local(a));
}

template <char T>
void jni_get_elements(Ctx& c) {
    c.a32();
    jarray a = static_cast<jarray>(obj(c.a32()));
    gaddr is_copy = c.a32();
    JNIEnv* e = env();
    jsize n = e->GetArrayLength(a);
    gaddr buf = gmalloc(u32(n) * elem_size(T) + 8);
    if (n) array_region(e, T, false, a, 0, n, h(buf));
    if (is_copy) wr<u8>(is_copy, 1);
    c.ret(buf);
}

template <char T>
void jni_release_elements(Ctx& c) {
    c.a32();
    jarray a = static_cast<jarray>(obj(c.a32()));
    gaddr buf = c.a32();
    jint mode = jint(c.a32());
    JNIEnv* e = env();
    if (mode != JNI_ABORT) {
        jsize n = e->GetArrayLength(a);
        if (n) array_region(e, T, true, a, 0, n, h(buf));
    }
    if (mode != JNI_COMMIT) gfree(buf);
}

template <char T, bool Set>
void jni_region(Ctx& c) {
    c.a32();
    jarray a = static_cast<jarray>(obj(c.a32()));
    jsize start = jsize(c.a32()), len = jsize(c.a32());
    gaddr buf = c.a32();
    if (len > 0) array_region(env(), T, Set, a, start, len, h(buf));
}

std::mutex g_crit_mu;
std::unordered_map<gaddr, char> g_crit_types;

char array_type(jarray a) {
    JNIEnv* e = env();
    jclass cls = e->GetObjectClass(a);
    jclass clsclass = e->GetObjectClass(cls);
    jmethodID getName = e->GetMethodID(clsclass, "getName", "()Ljava/lang/String;");
    auto name = static_cast<jstring>(e->CallObjectMethod(cls, getName));
    const char* s = e->GetStringUTFChars(name, nullptr);
    char t = s && s[0] == '[' ? s[1] : 'B';
    e->ReleaseStringUTFChars(name, s);
    e->DeleteLocalRef(name);
    e->DeleteLocalRef(clsclass);
    e->DeleteLocalRef(cls);
    return t;
}

void jni_get_critical(Ctx& c) {
    c.a32();
    jarray a = static_cast<jarray>(obj(c.a32()));
    gaddr is_copy = c.a32();
    char t = array_type(a);
    JNIEnv* e = env();
    jsize n = e->GetArrayLength(a);
    gaddr buf = gmalloc(u32(n) * elem_size(t) + 8);
    if (n) array_region(e, t, false, a, 0, n, h(buf));
    if (is_copy) wr<u8>(is_copy, 1);
    std::lock_guard lk(g_crit_mu);
    g_crit_types[buf] = t;
    c.ret(buf);
}

void jni_release_critical(Ctx& c) {
    c.a32();
    jarray a = static_cast<jarray>(obj(c.a32()));
    gaddr buf = c.a32();
    jint mode = jint(c.a32());
    char t;
    {
        std::lock_guard lk(g_crit_mu);
        t = g_crit_types[buf];
        if (mode != JNI_COMMIT) g_crit_types.erase(buf);
    }
    JNIEnv* e = env();
    if (mode != JNI_ABORT) {
        jsize n = e->GetArrayLength(a);
        if (n) array_region(e, t, true, a, 0, n, h(buf));
    }
    if (mode != JNI_COMMIT) gfree(buf);
}

// ------------------------------------------------------------------ everything else

void jni_GetVersion(Ctx& c) { c.ret(JNI_VERSION_1_6); }

void jni_FindClass(Ctx& c) {
    c.a32();
    const char* name = hs(c.a32());
    c.ret(local(env()->FindClass(name)));
}
void jni_GetSuperclass(Ctx& c) {
    c.a32();
    c.ret(local(env()->GetSuperclass(static_cast<jclass>(obj(c.a32())))));
}
void jni_IsAssignableFrom(Ctx& c) {
    c.a32();
    auto a = static_cast<jclass>(obj(c.a32()));
    auto b = static_cast<jclass>(obj(c.a32()));
    c.ret(env()->IsAssignableFrom(a, b));
}
void jni_Throw(Ctx& c) {
    c.a32();
    c.ret(u32(env()->Throw(static_cast<jthrowable>(obj(c.a32())))));
}
void jni_ThrowNew(Ctx& c) {
    c.a32();
    auto cls = static_cast<jclass>(obj(c.a32()));
    c.ret(u32(env()->ThrowNew(cls, hs(c.a32()))));
}
void jni_ExceptionOccurred(Ctx& c) { c.ret(local(env()->ExceptionOccurred())); }
void jni_ExceptionDescribe(Ctx&) { env()->ExceptionDescribe(); }
void jni_ExceptionClear(Ctx&) { env()->ExceptionClear(); }
void jni_ExceptionCheck(Ctx& c) { c.ret(env()->ExceptionCheck()); }
void jni_FatalError(Ctx& c) {
    c.a32();
    const char* msg = hs(c.a32());
    c.cpu.dump("FatalError");
    fatal("guest JNI FatalError: %s", msg ? msg : "");
}
void jni_PushLocalFrame(Ctx& c) {
    c.a32();
    jint r = env()->PushLocalFrame(jint(c.a32()));
    if (r == 0) t_frames.push_back({g_frame_ids++, {}});
    c.ret(u32(r));
}
void jni_PopLocalFrame(Ctx& c) {
    c.a32();
    jobject res = obj(c.a32());
    jobject parent = env()->PopLocalFrame(res);
    if (!t_frames.empty()) pop_frame();
    c.ret(local(parent));
}
void jni_NewGlobalRef(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    c.ret(o ? global(env()->NewGlobalRef(o), kGlobalFrame) : 0);
}
void jni_DeleteGlobalRef(Ctx& c) {
    c.a32();
    u32 hnd = c.a32();
    if (!hnd) return;
    env()->DeleteGlobalRef(obj(hnd));
    free_slot(hnd >> 2);
}
void jni_NewWeakGlobalRef(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    c.ret(o ? global(env()->NewWeakGlobalRef(o), kWeakFrame) : 0);
}
void jni_DeleteWeakGlobalRef(Ctx& c) {
    c.a32();
    u32 hnd = c.a32();
    if (!hnd) return;
    env()->DeleteWeakGlobalRef(static_cast<jweak>(obj(hnd)));
    free_slot(hnd >> 2);
}
void jni_DeleteLocalRef(Ctx& c) {
    c.a32();
    u32 hnd = c.a32();
    if (!hnd) return;
    jobject o = obj(hnd);
    u32 frame = g_slots[hnd >> 2].frame;
    if (frame == kGlobalFrame || frame == kWeakFrame) {
        NB_LOGW("guest DeleteLocalRef on a global reference %#x ignored", hnd);
        return;
    }
    if (o) env()->DeleteLocalRef(o);
    free_slot(hnd >> 2);
}
void jni_IsSameObject(Ctx& c) {
    c.a32();
    jobject a = obj(c.a32()), b = obj(c.a32());
    c.ret(env()->IsSameObject(a, b));
}
void jni_NewLocalRef(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    c.ret(o ? local(env()->NewLocalRef(o)) : 0);
}
void jni_EnsureLocalCapacity(Ctx& c) {
    c.a32();
    c.ret(u32(env()->EnsureLocalCapacity(jint(c.a32()))));
}
void jni_AllocObject(Ctx& c) {
    c.a32();
    c.ret(local(env()->AllocObject(static_cast<jclass>(obj(c.a32())))));
}
void jni_GetObjectClass(Ctx& c) {
    c.a32();
    c.ret(local(env()->GetObjectClass(obj(c.a32()))));
}
void jni_IsInstanceOf(Ctx& c) {
    c.a32();
    jobject o = obj(c.a32());
    c.ret(env()->IsInstanceOf(o, static_cast<jclass>(obj(c.a32()))));
}
template <bool Static>
void jni_GetMethodID(Ctx& c) {
    c.a32();
    auto cls = static_cast<jclass>(obj(c.a32()));
    const char* name = hs(c.a32());
    const char* sig = hs(c.a32());
    jmethodID m = Static ? env()->GetStaticMethodID(cls, name, sig) : env()->GetMethodID(cls, name, sig);
    c.ret(method_handle(m, sig));
}
template <bool Static>
void jni_GetFieldID(Ctx& c) {
    c.a32();
    auto cls = static_cast<jclass>(obj(c.a32()));
    const char* name = hs(c.a32());
    const char* sig = hs(c.a32());
    jfieldID f = Static ? env()->GetStaticFieldID(cls, name, sig) : env()->GetFieldID(cls, name, sig);
    c.ret(field_handle(f, sig));
}

void jni_NewString(Ctx& c) {
    c.a32();
    const jchar* s = hp<const jchar>(c.a32());
    c.ret(local(env()->NewString(s, jsize(c.a32()))));
}
void jni_GetStringLength(Ctx& c) {
    c.a32();
    c.ret(u32(env()->GetStringLength(static_cast<jstring>(obj(c.a32())))));
}
void jni_GetStringChars(Ctx& c) {
    c.a32();
    auto s = static_cast<jstring>(obj(c.a32()));
    gaddr is_copy = c.a32();
    JNIEnv* e = env();
    jsize n = e->GetStringLength(s);
    gaddr buf = gcalloc(u32(n) + 1, 2);
    if (n) e->GetStringRegion(s, 0, n, hp<jchar>(buf));
    if (is_copy) wr<u8>(is_copy, 1);
    c.ret(buf);
}
void jni_ReleaseStringChars(Ctx& c) {
    c.a32();
    c.a32();
    gfree(c.a32());
}
void jni_NewStringUTF(Ctx& c) {
    c.a32();
    gaddr s = c.a32();
    c.ret(s ? local(env()->NewStringUTF(hs(s))) : 0);
}
void jni_GetStringUTFLength(Ctx& c) {
    c.a32();
    c.ret(u32(env()->GetStringUTFLength(static_cast<jstring>(obj(c.a32())))));
}
void jni_GetStringUTFChars(Ctx& c) {
    c.a32();
    auto s = static_cast<jstring>(obj(c.a32()));
    gaddr is_copy = c.a32();
    JNIEnv* e = env();
    const char* chars = e->GetStringUTFChars(s, nullptr);
    if (!chars) {
        c.ret(0);
        return;
    }
    gaddr buf = gstrdup(chars);
    e->ReleaseStringUTFChars(s, chars);
    if (is_copy) wr<u8>(is_copy, 1);
    c.ret(buf);
}
void jni_ReleaseStringUTFChars(Ctx& c) {
    c.a32();
    c.a32();
    gfree(c.a32());
}
void jni_GetStringRegion(Ctx& c) {
    c.a32();
    auto s = static_cast<jstring>(obj(c.a32()));
    jsize start = jsize(c.a32()), len = jsize(c.a32());
    env()->GetStringRegion(s, start, len, hp<jchar>(c.a32()));
}
void jni_GetStringUTFRegion(Ctx& c) {
    c.a32();
    auto s = static_cast<jstring>(obj(c.a32()));
    jsize start = jsize(c.a32()), len = jsize(c.a32());
    env()->GetStringUTFRegion(s, start, len, hp<char>(c.a32()));
}
void jni_GetArrayLength(Ctx& c) {
    c.a32();
    c.ret(u32(env()->GetArrayLength(static_cast<jarray>(obj(c.a32())))));
}
void jni_NewObjectArray(Ctx& c) {
    c.a32();
    jsize n = jsize(c.a32());
    auto cls = static_cast<jclass>(obj(c.a32()));
    jobject init = obj(c.a32());
    c.ret(local(env()->NewObjectArray(n, cls, init)));
}
void jni_GetObjectArrayElement(Ctx& c) {
    c.a32();
    auto a = static_cast<jobjectArray>(obj(c.a32()));
    c.ret(local(env()->GetObjectArrayElement(a, jsize(c.a32()))));
}
void jni_SetObjectArrayElement(Ctx& c) {
    c.a32();
    auto a = static_cast<jobjectArray>(obj(c.a32()));
    jsize i = jsize(c.a32());
    env()->SetObjectArrayElement(a, i, obj(c.a32()));
}
void jni_RegisterNatives(Ctx& c) {
    c.cpu.dump("RegisterNatives");
    fatal("guest JNI RegisterNatives is not supported");
}
void jni_UnregisterNatives(Ctx& c) { c.ret(0); }
void jni_MonitorEnter(Ctx& c) {
    c.a32();
    c.ret(u32(env()->MonitorEnter(obj(c.a32()))));
}
void jni_MonitorExit(Ctx& c) {
    c.a32();
    c.ret(u32(env()->MonitorExit(obj(c.a32()))));
}
void jni_GetJavaVM(Ctx& c) {
    c.a32();
    gaddr out = c.a32();
    wr<u32>(out, g_guest_vm);
    c.ret(0);
}
void jni_GetStringCritical(Ctx& c) { jni_GetStringChars(c); }
void jni_ReleaseStringCritical(Ctx& c) { jni_ReleaseStringChars(c); }
void jni_NewDirectByteBuffer(Ctx& c) {
    c.a32();
    gaddr p = c.a32();
    jlong cap = jlong(c.a64());
    c.ret(local(env()->NewDirectByteBuffer(h(p), cap)));
}
void jni_GetDirectBufferAddress(Ctx& c) {
    c.a32();
    void* p = env()->GetDirectBufferAddress(obj(c.a32()));
    if (p && !in_guest(p)) {
        NB_LOGW("GetDirectBufferAddress: buffer lives outside guest memory");
        p = nullptr;
    }
    c.ret(g(p));
}
void jni_GetDirectBufferCapacity(Ctx& c) {
    c.a32();
    c.ret64(u64(env()->GetDirectBufferCapacity(obj(c.a32()))));
}
void jni_GetObjectRefType(Ctx& c) {
    c.a32();
    c.ret(u32(env()->GetObjectRefType(obj(c.a32()))));
}

// ------------------------------------------------------------------ JavaVM

void vm_DestroyJavaVM(Ctx& c) {
    c.cpu.dump("DestroyJavaVM");
    fatal("guest called DestroyJavaVM");
}
void vm_AttachCurrentThread(Ctx& c) {
    c.a32();
    gaddr penv = c.a32();
    JNIEnv* e = nullptr;
#ifdef __ANDROID__
    jint r = g_vm->AttachCurrentThread(&e, nullptr);
#else
    jint r = g_vm->AttachCurrentThread(reinterpret_cast<void**>(&e), nullptr);
#endif
    if (r == JNI_OK) {
        t_env = e;
        if (penv) wr<u32>(penv, g_guest_env);
    }
    c.ret(u32(r));
}
void vm_DetachCurrentThread(Ctx& c) {
    while (!t_frames.empty()) pop_frame();
    t_env = nullptr;
    c.ret(u32(g_vm->DetachCurrentThread()));
}
void vm_GetEnv(Ctx& c) {
    c.a32();
    gaddr penv = c.a32();
    jint version = jint(c.a32());
    JNIEnv* e = nullptr;
    jint r = g_vm->GetEnv(reinterpret_cast<void**>(&e), version);
    if (penv) wr<u32>(penv, r == JNI_OK ? g_guest_env : 0);
    if (r == JNI_OK) t_env = e;
    c.ret(u32(r));
}

void jni_missing(Ctx& c) {
    c.cpu.dump("missing JNI function");
    fatal("guest called unimplemented JNI function %s", c.name);
}

void set_entry(u32 index, const char* name, Handler fn) { wr<u32>(g_env_table + index * 4, make_stub(name, fn)); }

}  // namespace

void jni_set_vm(JavaVM* vm) { g_vm = vm; }
gaddr jni_guest_env() { return g_guest_env; }
JNIEnv* jni_env() { return env(); }
u32 jni_local(jobject o) { return local(o); }
jobject jni_obj(u32 hnd) { return obj(hnd); }

JniFrame::JniFrame(JNIEnv* e) {
    t_env = e;
    t_frames.push_back({g_frame_ids++, {}});
}
JniFrame::~JniFrame() { pop_frame(); }

void register_jni() {
    g_slots = static_cast<Slot*>(calloc(kMaxRefs, sizeof(Slot)));

    constexpr u32 kEntries = sizeof(HostNI) / sizeof(void*);
    g_env_table = misc_alloc(kEntries * 4, 8);
    g_guest_env = misc_alloc(4);
    wr<u32>(g_guest_env, g_env_table);
    for (u32 i = 0; i < kEntries; ++i) {
        std::string nm = "JNI#" + std::to_string(i);
        set_entry(i, strdup(nm.c_str()), jni_missing);
    }

#define E(fn) set_entry(JNI_INDEX(fn), "JNI::" #fn, jni_##fn)
    E(GetVersion);
    E(FindClass);
    E(GetSuperclass);
    E(IsAssignableFrom);
    E(Throw);
    E(ThrowNew);
    E(ExceptionOccurred);
    E(ExceptionDescribe);
    E(ExceptionClear);
    E(ExceptionCheck);
    E(FatalError);
    E(PushLocalFrame);
    E(PopLocalFrame);
    E(NewGlobalRef);
    E(DeleteGlobalRef);
    E(NewWeakGlobalRef);
    E(DeleteWeakGlobalRef);
    E(DeleteLocalRef);
    E(IsSameObject);
    E(NewLocalRef);
    E(EnsureLocalCapacity);
    E(AllocObject);
    E(GetObjectClass);
    E(IsInstanceOf);
    E(NewString);
    E(GetStringLength);
    E(GetStringChars);
    E(ReleaseStringChars);
    E(NewStringUTF);
    E(GetStringUTFLength);
    E(GetStringUTFChars);
    E(ReleaseStringUTFChars);
    E(GetStringRegion);
    E(GetStringUTFRegion);
    E(GetStringCritical);
    E(ReleaseStringCritical);
    E(GetArrayLength);
    E(NewObjectArray);
    E(GetObjectArrayElement);
    E(SetObjectArrayElement);
    E(RegisterNatives);
    E(UnregisterNatives);
    E(MonitorEnter);
    E(MonitorExit);
    E(GetJavaVM);
    E(NewDirectByteBuffer);
    E(GetDirectBufferAddress);
    E(GetDirectBufferCapacity);
    E(GetObjectRefType);
#undef E
    set_entry(JNI_INDEX(GetMethodID), "JNI::GetMethodID", jni_GetMethodID<false>);
    set_entry(JNI_INDEX(GetStaticMethodID), "JNI::GetStaticMethodID", jni_GetMethodID<true>);
    set_entry(JNI_INDEX(GetFieldID), "JNI::GetFieldID", jni_GetFieldID<false>);
    set_entry(JNI_INDEX(GetStaticFieldID), "JNI::GetStaticFieldID", jni_GetFieldID<true>);
    set_entry(JNI_INDEX(NewObject), "JNI::NewObject", jni_new_object<kDots>);
    set_entry(JNI_INDEX(NewObjectV), "JNI::NewObjectV", jni_new_object<kV>);
    set_entry(JNI_INDEX(NewObjectA), "JNI::NewObjectA", jni_new_object<kA>);
    set_entry(JNI_INDEX(GetPrimitiveArrayCritical), "JNI::GetPrimitiveArrayCritical", jni_get_critical);
    set_entry(JNI_INDEX(ReleasePrimitiveArrayCritical), "JNI::ReleasePrimitiveArrayCritical", jni_release_critical);

#define CALLS(Ty, ch)                                                                                         \
    set_entry(JNI_INDEX(Call##Ty##Method), "JNI::Call" #Ty "Method", jni_call<kVirtual, kDots, ch>);            \
    set_entry(JNI_INDEX(Call##Ty##MethodV), "JNI::Call" #Ty "MethodV", jni_call<kVirtual, kV, ch>);             \
    set_entry(JNI_INDEX(Call##Ty##MethodA), "JNI::Call" #Ty "MethodA", jni_call<kVirtual, kA, ch>);             \
    set_entry(JNI_INDEX(CallNonvirtual##Ty##Method), "JNI::CallNonvirtual" #Ty "Method",                       \
              jni_call<kNonvirtual, kDots, ch>);                                                              \
    set_entry(JNI_INDEX(CallNonvirtual##Ty##MethodV), "JNI::CallNonvirtual" #Ty "MethodV",                     \
              jni_call<kNonvirtual, kV, ch>);                                                                 \
    set_entry(JNI_INDEX(CallNonvirtual##Ty##MethodA), "JNI::CallNonvirtual" #Ty "MethodA",                     \
              jni_call<kNonvirtual, kA, ch>);                                                                 \
    set_entry(JNI_INDEX(CallStatic##Ty##Method), "JNI::CallStatic" #Ty "Method", jni_call<kStatic, kDots, ch>); \
    set_entry(JNI_INDEX(CallStatic##Ty##MethodV), "JNI::CallStatic" #Ty "MethodV", jni_call<kStatic, kV, ch>);  \
    set_entry(JNI_INDEX(CallStatic##Ty##MethodA), "JNI::CallStatic" #Ty "MethodA", jni_call<kStatic, kA, ch>);
    CALLS(Object, 'L')
    CALLS(Boolean, 'Z')
    CALLS(Byte, 'B')
    CALLS(Char, 'C')
    CALLS(Short, 'S')
    CALLS(Int, 'I')
    CALLS(Long, 'J')
    CALLS(Float, 'F')
    CALLS(Double, 'D')
    CALLS(Void, 'V')
#undef CALLS

#define FIELDS(Ty, ch)                                                                                     \
    set_entry(JNI_INDEX(Get##Ty##Field), "JNI::Get" #Ty "Field", jni_get_field<ch, false>);               \
    set_entry(JNI_INDEX(Set##Ty##Field), "JNI::Set" #Ty "Field", jni_set_field<ch, false>);               \
    set_entry(JNI_INDEX(GetStatic##Ty##Field), "JNI::GetStatic" #Ty "Field", jni_get_field<ch, true>);    \
    set_entry(JNI_INDEX(SetStatic##Ty##Field), "JNI::SetStatic" #Ty "Field", jni_set_field<ch, true>);
    FIELDS(Object, 'L')
    FIELDS(Boolean, 'Z')
    FIELDS(Byte, 'B')
    FIELDS(Char, 'C')
    FIELDS(Short, 'S')
    FIELDS(Int, 'I')
    FIELDS(Long, 'J')
    FIELDS(Float, 'F')
    FIELDS(Double, 'D')
#undef FIELDS

#define ARRAYS(Ty, ch)                                                                                              \
    set_entry(JNI_INDEX(New##Ty##Array), "JNI::New" #Ty "Array", jni_new_array<ch>);                               \
    set_entry(JNI_INDEX(Get##Ty##ArrayElements), "JNI::Get" #Ty "ArrayElements", jni_get_elements<ch>);            \
    set_entry(JNI_INDEX(Release##Ty##ArrayElements), "JNI::Release" #Ty "ArrayElements", jni_release_elements<ch>); \
    set_entry(JNI_INDEX(Get##Ty##ArrayRegion), "JNI::Get" #Ty "ArrayRegion", jni_region<ch, false>);               \
    set_entry(JNI_INDEX(Set##Ty##ArrayRegion), "JNI::Set" #Ty "ArrayRegion", jni_region<ch, true>);
    ARRAYS(Boolean, 'Z')
    ARRAYS(Byte, 'B')
    ARRAYS(Char, 'C')
    ARRAYS(Short, 'S')
    ARRAYS(Int, 'I')
    ARRAYS(Long, 'J')
    ARRAYS(Float, 'F')
    ARRAYS(Double, 'D')
#undef ARRAYS

    constexpr u32 kVmEntries = sizeof(HostII) / sizeof(void*);
    g_vm_table = misc_alloc(kVmEntries * 4, 8);
    g_guest_vm = misc_alloc(4);
    wr<u32>(g_guest_vm, g_vm_table);
    for (u32 i = 0; i < kVmEntries; ++i) wr<u32>(g_vm_table + i * 4, make_stub("JavaVM#?", jni_missing));
    wr<u32>(g_vm_table + JII_INDEX(DestroyJavaVM) * 4, make_stub("JavaVM::DestroyJavaVM", vm_DestroyJavaVM));
    wr<u32>(g_vm_table + JII_INDEX(AttachCurrentThread) * 4,
            make_stub("JavaVM::AttachCurrentThread", vm_AttachCurrentThread));
    wr<u32>(g_vm_table + JII_INDEX(DetachCurrentThread) * 4,
            make_stub("JavaVM::DetachCurrentThread", vm_DetachCurrentThread));
    wr<u32>(g_vm_table + JII_INDEX(GetEnv) * 4, make_stub("JavaVM::GetEnv", vm_GetEnv));
    wr<u32>(g_vm_table + JII_INDEX(AttachCurrentThreadAsDaemon) * 4,
            make_stub("JavaVM::AttachCurrentThreadAsDaemon", vm_AttachCurrentThread));
}

}  // namespace nb
