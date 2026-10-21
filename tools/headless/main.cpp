// Headless, deterministic run of the guest keyboard engine: fake JNI, recording GL, fake clock.
// Prints a hash of the GL command stream per frame so different JIT backends can be compared.
//   ninheadless <assets-dir> <home-dir> [frames]
#include <jni.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
int64_t nb_fake_time_us = 1700000000LL * 1000000;
JNIEnv* fake_jni_env(const char* home, std::string* ops);
JavaVM* fake_jni_vm();
jclass fake_jni_class(const char* name);
jstring fake_jni_string(const char* s);
uint64_t fakegl_take_hash(uint64_t* calls, uint64_t* draws);
void fakegl_trace(FILE* f);

jint JNI_OnLoad(JavaVM*, void*);
void Java_com_jormy_nin_NINLib_init(JNIEnv*, jclass, jint, jint, jint, jint);
void Java_com_jormy_nin_NINLib_step(JNIEnv*, jclass);
jlong Java_com_jormy_nin_NINLib_syncTiming(JNIEnv*, jclass, jlong);
void Java_com_jormy_nin_NINLib_onTouchEvent(JNIEnv*, jclass, jint, jint, jfloat, jfloat, jfloat, jfloat, jlong);
void Java_com_jormy_nin_NINLib_onChangeAppOrTextbox(JNIEnv*, jclass, jstring, jstring, jstring);
void Java_com_jormy_nin_NINLib_onTextSelection(JNIEnv*, jclass, jstring, jstring, jstring, jstring);
jint Java_com_jormy_nin_NINLib_getUnicodeBackIndex(JNIEnv*, jclass, jstring, jint);
jlong Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementLeft(JNIEnv*, jclass, jstring);
}

static JNIEnv* env;
static jclass cls;
static int frame_no = 0;

static int64_t now_ms() { return nb_fake_time_us / 1000; }

static void frame() {
    static int trace_frame = getenv("FAKEGL_TRACE_FRAME") ? atoi(getenv("FAKEGL_TRACE_FRAME")) : -1;
    if (frame_no == trace_frame) fakegl_trace(stderr);
    Java_com_jormy_nin_NINLib_step(env, cls);
    fakegl_trace(nullptr);
    nb_fake_time_us += 16667;
    uint64_t calls, draws;
    uint64_t hsh = fakegl_take_hash(&calls, &draws);
    printf("frame %4d  gl=%016" PRIx64 "  calls=%" PRIu64 " draws=%" PRIu64 "\n", frame_no++, hsh, calls, draws);
}

static void tap(float x, float y) {
    Java_com_jormy_nin_NINLib_onTouchEvent(env, cls, 0, 0, x, y, 1.f, 0.1f, now_ms());
    for (int i = 0; i < 3; ++i) frame();
    Java_com_jormy_nin_NINLib_onTouchEvent(env, cls, 0, 2, x, y, 1.f, 0.1f, now_ms());
    for (int i = 0; i < 6; ++i) frame();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <assets-dir> <home-dir> [frames]\n", argv[0]);
        return 2;
    }
    setenv("NIN_ASSETS", argv[1], 1);
    int extra = argc > 3 ? atoi(argv[3]) : 60;
    std::string ops;
    env = fake_jni_env(argv[2], &ops);
    cls = fake_jni_class("com/jormy/nin/NINLib");
    setvbuf(stdout, nullptr, _IOLBF, 0);

    JNI_OnLoad(fake_jni_vm(), nullptr);
    Java_com_jormy_nin_NINLib_syncTiming(env, cls, now_ms());
    Java_com_jormy_nin_NINLib_init(env, cls, 1080, 760, 1080, 2340);
    uint64_t c, d;
    printf("init  gl=%016" PRIx64 "\n", fakegl_take_hash(&c, &d));
    Java_com_jormy_nin_NINLib_onChangeAppOrTextbox(env, cls, fake_jni_string("com.example.notes"),
                                                   fake_jni_string("body"), fake_jni_string("text"));
    Java_com_jormy_nin_NINLib_onTextSelection(env, cls, fake_jni_string("Hello "), fake_jni_string(""),
                                              fake_jni_string(""), fake_jni_string(""));
    for (int i = 0; i < 30; ++i) frame();
    const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const float geom[3][3] = {{0.050f, 0.0985f, 0.50f}, {0.089f, 0.0995f, 0.64f}, {0.198f, 0.0995f, 0.785f}};
    for (const char* word : {"howdy", "hello"}) {
        for (const char* p = word; *p; ++p)
            for (int r = 0; r < 3; ++r)
                for (int i = 0; rows[r][i]; ++i)
                    if (rows[r][i] == *p) tap((geom[r][0] + i * geom[r][1]) * 1080, geom[r][2] * 760);
    }
    for (int i = 0; i < extra; ++i) frame();
    printf("backIndex=%d cursorLeft=%" PRId64 "\n",
           Java_com_jormy_nin_NINLib_getUnicodeBackIndex(env, cls, fake_jni_string("hello"), 3),
           int64_t(Java_com_jormy_nin_NINLib_processSoftKeyboardCursorMovementLeft(env, cls,
                                                                                  fake_jni_string("hello world"))));
    printf("ops:\n%s", ops.c_str());
    return 0;
}
