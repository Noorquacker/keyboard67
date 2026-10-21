// Host-side JNI exports for com.jormy.nin.NINLib. Each forwards to the function of the same name
// inside the guest library, translating references and packing arguments per the ARM AAPCS.
#include <jni.h>

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

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    jni_set_vm(vm);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    JniFrame frame(env);
    runtime_init();
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
    call_guest(fn, {genv, gcls, ga(u32(id)), ga(u32(action)), gaf(x), gaf(y), gaf(pressure), gaf(area),
                    ga64(u64(ts))});
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
    call_guest(fn, {genv, gcls});
}

JNIEXPORT jlong JNICALL Java_com_jormy_nin_NINLib_syncTiming(JNIEnv* env, jclass cls, jlong t) {
    GUEST_FN(Java_com_jormy_nin_NINLib_syncTiming)
    return jlong(call_guest(fn, {genv, gcls, ga64(u64(t))}));
}

}  // extern "C"
