// Offscreen EGL/GLES2 context for the desktop harness.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

JNIEXPORT jboolean JNICALL Java_com_jormy_nin_HostTest_glInit(JNIEnv* env, jclass cls, jint w, jint h) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) return JNI_FALSE;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) return JNI_FALSE;
    const EGLint pb_attr[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pb_attr);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c)) return JNI_FALSE;
    printf("[hostgl] %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_jormy_nin_HostTest_glSave(JNIEnv* env, jclass cls, jstring jpath, jint w, jint h) {
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    unsigned char* px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
    free(px);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
}
