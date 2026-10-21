// Minimal in-process JNIEnv/JavaVM for headless runs (no JVM). Implements what the bridge uses
// and answers the keyboard's upcalls the way the app's Java classes would.
#include <jni.h>
#include <sys/stat.h>
#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace {

struct Obj {
    enum Kind { Class, String, Other } kind;
    std::string s;  // class name or string contents
};
struct Method {
    std::string cls, name, sig;
};

jobject mk(Obj::Kind k, std::string s) { return reinterpret_cast<jobject>(new Obj{k, std::move(s)}); }
Obj* O(jobject o) { return reinterpret_cast<Obj*>(o); }

std::string g_home;
std::string* g_ops;

template <size_t N>
void trap() {
    fprintf(stderr, "fake JNI: unimplemented JNINativeInterface entry #%zu\n", N);
    abort();
}
template <size_t... I>
void fill(void** t, std::index_sequence<I...>) {
    ((t[I] = reinterpret_cast<void*>(&trap<I>)), ...);
}

const std::pair<const char*, const char*> kReflection[] = {
#include "reflection.inc"
};

std::string arg_str(const jvalue& v) { return v.l ? O(v.l)->s : "null"; }

jvalue dispatch(jmethodID mid, const jvalue* a) {
    auto* m = reinterpret_cast<Method*>(mid);
    std::string full = m->cls + "." + m->name;
    jvalue r{};
    if (full == "com/jormy/nin/Utils.homeDirectory") {
        r.l = mk(Obj::String, g_home);
    } else if (full == "com/jormy/nin/Utils.cacheDirectory") {
        mkdir((g_home + "/cache").c_str(), 0755);
        r.l = mk(Obj::String, g_home + "/cache");
    } else if (full == "com/jormy/nin/Utils.assetManager") {
        r.l = mk(Obj::Other, "AssetManager");
    } else if (full == "com/jormy/nin/Utils.getIPAddressIPV4") {
        r.l = mk(Obj::String, "127.0.0.1");
    } else if (full == "com/jormy/nin/Utils.getStringFromClipboard") {
        r.l = mk(Obj::String, "");
    } else if (full == "com/jormy/nin/Utils.listAssetFiles") {
        std::string p = std::string(getenv("NIN_ASSETS")) + "/" + arg_str(a[0]);
        DIR* d = opendir(p.c_str());
        int n = 0;
        if (d) {
            while (readdir(d)) ++n;
            closedir(d);
        }
        r.z = n > 2;
    } else if (full == "com/jormy/nin/Utils.roenObtainSound") {
        r.l = mk(Obj::Other, "Integer");
    } else if (full == "java/lang/Integer.intValue") {
        r.i = 0;
    } else if (full == "com/jormy/nin/NINView.getDevicePPI") {
        r.f = 400.f;
    } else if (full == "com/jormy/nin/NINView.getDevicePortraitWidth") {
        r.f = 1080.f;
    } else if (full == "com/jormy/nin/Reflector.analyzeClass") {
        std::string out = "-- class not found!";
        for (auto& [name, methods] : kReflection)
            if (arg_str(a[0]) == name) out = methods;
        r.l = mk(Obj::String, out);
    } else if (m->cls == "com/jormy/nin/SoftKeyboard") {
        // Record the text operation with its arguments as the app would receive them.
        std::string op = m->name;
        const char* p = m->sig.c_str() + 1;
        for (int i = 0; *p && *p != ')'; ++i) {
            char t = *p;
            if (t == 'L') {
                op += " [" + arg_str(a[i]) + "]";
                while (*p != ';') ++p;
            } else if (t == 'Z') {
                op += a[i].z ? " true" : " false";
            } else {
                op += " " + std::to_string(a[i].i);
            }
            ++p;
        }
        *g_ops += op + "\n";
    } else if (m->sig.back() != 'V') {
        fprintf(stderr, "fake JNI: unhandled non-void upcall %s%s\n", full.c_str(), m->sig.c_str());
    }
    return r;
}

JNINativeInterface g_ni;
_JNIEnv g_env;
JNIInvokeInterface g_ii;
_JavaVM g_vm;

}  // namespace

extern "C" JNIEnv* fake_jni_env(const char* home, std::string* ops) {
    g_home = home;
    g_ops = ops;
    mkdir(home, 0755);
    fill(reinterpret_cast<void**>(&g_ni), std::make_index_sequence<sizeof(g_ni) / sizeof(void*)>{});
    auto& n = g_ni;
    n.GetVersion = [](JNIEnv*) -> jint { return JNI_VERSION_1_6; };
    n.FindClass = [](JNIEnv*, const char* name) -> jclass { return static_cast<jclass>(mk(Obj::Class, name)); };
    n.ExceptionCheck = [](JNIEnv*) -> jboolean { return JNI_FALSE; };
    n.ExceptionOccurred = [](JNIEnv*) -> jthrowable { return nullptr; };
    n.ExceptionClear = [](JNIEnv*) {};
    n.ExceptionDescribe = [](JNIEnv*) {};
    n.PushLocalFrame = [](JNIEnv*, jint) -> jint { return 0; };
    n.PopLocalFrame = [](JNIEnv*, jobject o) -> jobject { return o; };
    n.NewGlobalRef = [](JNIEnv*, jobject o) -> jobject { return o; };
    n.DeleteGlobalRef = [](JNIEnv*, jobject) {};
    n.NewWeakGlobalRef = [](JNIEnv*, jobject o) -> jweak { return o; };
    n.DeleteWeakGlobalRef = [](JNIEnv*, jweak) {};
    n.DeleteLocalRef = [](JNIEnv*, jobject) {};
    n.NewLocalRef = [](JNIEnv*, jobject o) -> jobject { return o; };
    n.EnsureLocalCapacity = [](JNIEnv*, jint) -> jint { return 0; };
    n.IsSameObject = [](JNIEnv*, jobject a, jobject b) -> jboolean { return a == b; };
    n.GetObjectClass = [](JNIEnv*, jobject o) -> jclass {
        return static_cast<jclass>(mk(Obj::Class, O(o)->kind == Obj::String ? "java/lang/String" : "java/lang/Object"));
    };
    n.IsInstanceOf = [](JNIEnv*, jobject, jclass) -> jboolean { return JNI_TRUE; };
    n.GetMethodID = [](JNIEnv*, jclass c, const char* name, const char* sig) -> jmethodID {
        std::string cls = O(c)->s;
        if (std::string(name) == "intValue") cls = "java/lang/Integer";
        return reinterpret_cast<jmethodID>(new Method{cls, name, sig});
    };
    n.GetStaticMethodID = [](JNIEnv*, jclass c, const char* name, const char* sig) -> jmethodID {
        return reinterpret_cast<jmethodID>(new Method{O(c)->s, name, sig});
    };
#define CALLS(T, f)                                                                                       \
    n.Call##T##MethodA = [](JNIEnv*, jobject, jmethodID m, const jvalue* a) { return dispatch(m, a).f; };   \
    n.CallStatic##T##MethodA = [](JNIEnv*, jclass, jmethodID m, const jvalue* a) { return dispatch(m, a).f; };
    CALLS(Object, l)
    CALLS(Boolean, z)
    CALLS(Byte, b)
    CALLS(Char, c)
    CALLS(Short, s)
    CALLS(Int, i)
    CALLS(Long, j)
    CALLS(Float, f)
    CALLS(Double, d)
#undef CALLS
    n.CallVoidMethodA = [](JNIEnv*, jobject, jmethodID m, const jvalue* a) { dispatch(m, a); };
    n.CallStaticVoidMethodA = [](JNIEnv*, jclass, jmethodID m, const jvalue* a) { dispatch(m, a); };
    n.NewStringUTF = [](JNIEnv*, const char* s) -> jstring { return static_cast<jstring>(mk(Obj::String, s)); };
    n.GetStringUTFLength = [](JNIEnv*, jstring s) -> jsize { return jsize(O(s)->s.size()); };
    n.GetStringLength = [](JNIEnv*, jstring s) -> jsize { return jsize(O(s)->s.size()); };  // ASCII only
    n.GetStringRegion = [](JNIEnv*, jstring s, jsize st, jsize len, jchar* out) {
        for (jsize i = 0; i < len; ++i) out[i] = jchar(static_cast<unsigned char>(O(s)->s[size_t(st + i)]));
    };
    n.GetStringUTFChars = [](JNIEnv*, jstring s, jboolean*) -> const char* { return strdup(O(s)->s.c_str()); };
    n.ReleaseStringUTFChars = [](JNIEnv*, jstring, const char* c) { free(const_cast<char*>(c)); };
    n.GetStringUTFRegion = [](JNIEnv*, jstring s, jsize st, jsize len, char* out) {
        memcpy(out, O(s)->s.data() + st, size_t(len));
    };

    g_env.functions = &g_ni;

    g_ii.GetEnv = [](JavaVM*, void** e, jint) -> jint {
        *e = &g_env;
        return JNI_OK;
    };
    g_ii.AttachCurrentThread = [](JavaVM*, JNIEnv** e, void*) -> jint {
        *e = &g_env;
        return JNI_OK;
    };
    g_ii.AttachCurrentThreadAsDaemon = g_ii.AttachCurrentThread;
    g_ii.DetachCurrentThread = [](JavaVM*) -> jint { return JNI_OK; };
    g_vm.functions = &g_ii;
    return &g_env;
}

extern "C" JavaVM* fake_jni_vm() { return &g_vm; }
extern "C" jclass fake_jni_class(const char* name) { return static_cast<jclass>(mk(Obj::Class, name)); }
extern "C" jstring fake_jni_string(const char* s) { return static_cast<jstring>(mk(Obj::String, s)); }
