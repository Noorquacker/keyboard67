// Host-side JNI exports for com.jormy.nin.NINLib. Each forwards to the function of the same name
// inside the guest library, translating references and packing arguments per the ARM AAPCS.
#include <jni.h>

#include <ctime>

#include "jni_bridge.h"
#include "nb.h"

using namespace nb;

namespace {

gaddr need(const char* name) {
    gaddr f = guest_sym(name);
    if (!f) fatal("guest library does not export %s", name);
    return f;
}

#define GUEST_FN(name)                     \
    static const gaddr fn = need(#name); \
    JniFrame frame(env);                   \
    const GArg genv = ga(jni_guest_env()); \
    const GArg gcls = ga(jni_local(cls));

GArg str(jstring s) { return ga(jni_local(s)); }

double now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// Logs how long the guest spends per frame (and in touch handling), every ~5 s.
struct FrameStats {
    double start = now_ms(), total = 0, worst = 0, touch = 0;
    int frames = 0, touches = 0;
    void frame(double ms) {
        total += ms;
        worst = ms > worst ? ms : worst;
        ++frames;
        double t = now_ms();
        if (t - start < 5000) return;
        NB_LOGI("perf: %d frames in %.1f s, step avg %.2f ms, max %.2f ms; %d touches avg %.2f ms", frames,
                (t - start) / 1e3, total / frames, worst, touches, touches ? touch / touches : 0.0);
        *this = {};
    }
} g_stats;

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    jni_set_vm(vm);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    {
        JniFrame frame(env);
        runtime_init();
        warmup_precompile();
    }
    // Usually this runs on the keyboard's background preload thread: hand the now-warm JIT to
    // whichever thread (the GL thread) calls into the engine next.
    release_current_guest_thread();
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_com_jormy_nin_NINLib_getUnicodeBackIndex(JNIEnv* env, jclass cls, jstring s, jint i) {
    GUEST_FN(Java_com_jormy_nin_NINLib_getUnicodeBackIndex)
    return jint(u32(call_guest(fn, {genv, gcls, str(s), ga(u32(i))})));
}

JNIEXPORT jint JNICALL Java_com_jormy_nin_NINLib_getUnicodeFrontIndex(JNIEnv* env, jclass cls, jstring s, jint i) {
    GUEST_FN(Java_com_jormy_nin_NINLib_getUnicodeFrontIndex)
    return jint(u32(call_guest(fn, {genv, gcls, str(s), ga(u32(i))})));
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_init(JNIEnv* env, jclass cls, jint a, jint b, jint c, jint d) {
    GUEST_FN(Java_com_jormy_nin_NINLib_init)
    call_guest(fn, {genv, gcls, ga(u32(a)), ga(u32(b)), ga(u32(c)), ga(u32(d))});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_memTestStep(JNIEnv* env, jclass cls) {
    GUEST_FN(Java_com_jormy_nin_NINLib_memTestStep)
    call_guest(fn, {genv, gcls});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onChangeAppOrTextbox(JNIEnv* env, jclass cls, jstring a, jstring b,
                                                                     jstring c) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onChangeAppOrTextbox)
    call_guest(fn, {genv, gcls, str(a), str(b), str(c)});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onEditorChangeTypeClass(JNIEnv* env, jclass cls, jstring a,
                                                                        jstring b) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onEditorChangeTypeClass)
    call_guest(fn, {genv, gcls, str(a), str(b)});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onExternalSelChange(JNIEnv* env, jclass cls) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onExternalSelChange)
    call_guest(fn, {genv, gcls});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onTextSelection(JNIEnv* env, jclass cls, jstring a, jstring b,
                                                                jstring c, jstring d) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onTextSelection)
    call_guest(fn, {genv, gcls, str(a), str(b), str(c), str(d)});
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onTouchEvent(JNIEnv* env, jclass cls, jint id, jint action, jfloat x,
                                                             jfloat y, jfloat pressure, jfloat area, jlong ts) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onTouchEvent)
    double t0 = now_ms();
    call_guest(fn, {genv, gcls, ga(u32(id)), ga(u32(action)), gaf(x), gaf(y), gaf(pressure), gaf(area),
                    ga64(u64(ts))});
    g_stats.touch += now_ms() - t0;
    ++g_stats.touches;
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_onWordDestruction(JNIEnv* env, jclass cls, jstring a, jstring b) {
    GUEST_FN(Java_com_jormy_nin_NINLib_onWordDestruction)
    call_guest(fn, {genv, gcls, str(a), str(b)});
}

JNIEXPORT jint JNICALL Java_com_jormy_nin_NINLib_processBackspaceAllowance(JNIEnv* env, jclass cls, jstring a,
                                                                          jstring b, jint i) {
    GUEST_FN(Java_com_jormy_nin_NINLib_processBackspaceAllowance)
    return jint(u32(call_guest(fn, {genv, gcls, str(a), str(b), ga(u32(i))})));
}

JNIEXPORT jlong JNICALL Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementLeft(JNIEnv* env, jclass cls,
                                                                                       jstring s) {
    GUEST_FN(Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementLeft)
    return jlong(call_guest(fn, {genv, gcls, str(s)}));
}

JNIEXPORT jlong JNICALL Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementRight(JNIEnv* env, jclass cls,
                                                                                        jstring s) {
    GUEST_FN(Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementRight)
    return jlong(call_guest(fn, {genv, gcls, str(s)}));
}

JNIEXPORT void JNICALL Java_com_jormy_nin_NINLib_step(JNIEnv* env, jclass cls) {
    GUEST_FN(Java_com_jormy_nin_NINLib_step)
    double t0 = now_ms();
    call_guest(fn, {genv, gcls});
    g_stats.frame(now_ms() - t0);
    warmup_note_frame();
}

// Not part of the original library: lets the view stop rendering while the picture is static.
JNIEXPORT jboolean JNICALL Java_com_jormy_nin_NINLib_frameChanged(JNIEnv*, jclass) {
    return gles_take_frame_changed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_jormy_nin_NINLib_syncTiming(JNIEnv* env, jclass cls, jlong t) {
    GUEST_FN(Java_com_jormy_nin_NINLib_syncTiming)
    return jlong(call_guest(fn, {genv, gcls, ga64(u64(t))}));
}

}  // extern "C"
