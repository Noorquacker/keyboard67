// OpenGL ES 2.0 imports. All prototypes use 32-bit scalar types, so most are bound directly with
// automatic pointer translation; the few with host-width or pointer-array parameters are wrapped.
#include <GLES2/gl2.h>

#include <mutex>
#include <unordered_map>

#include "nb.h"

namespace nb {
namespace {

thread_local GLuint t_array_buffer = 0;

// ---------------------------------------------------------------- frame fingerprint
// A hash of everything that affects the rendered image (state, uniforms, draws and the vertex data
// they read). If two consecutive frames hash the same, the picture is static and the Java side can
// stop requesting frames until something happens.

u64 g_frame_hash = 1469598103934665603ull;
u64 g_upload_nonce = 0;

inline void mix(const void* p, size_t n) {
    auto* b = static_cast<const u8*>(p);
    u64 x = g_frame_hash;
    for (size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 1099511628211ull;
    g_frame_hash = x;
}
template <class T>
inline void mixv(T v) {
    mix(&v, sizeof v);
}

struct Attrib {
    bool enabled = false;
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    const u8* client = nullptr;  // null when sourced from a VBO
};
Attrib g_attribs[16];

template <class R, class... A>
constexpr u32 arity(R (*)(A...)) {
    return u32(sizeof...(A));
}

// Thunk for scalar-only GL calls (every argument is one 32-bit word) that also folds the call and
// its arguments into the frame hash.
template <auto F>
void hashed(Ctx& c) {
    constexpr u32 n = arity(F);
    mixv(reinterpret_cast<uintptr_t>(F));
    for (u32 i = 0; i < n; ++i) mixv(i < 4 ? c.r[i] : rd<u32>(c.r[13] + (i - 4) * 4));
    auto_thunk<F>(c);
}

void x_glBindBuffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) t_array_buffer = buf;
    mixv(target);
    mixv(buf);
    glBindBuffer(target, buf);
}

void x_glEnableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = true;
    glEnableVertexAttribArray(i);
}
void x_glDisableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = false;
    glDisableVertexAttribArray(i);
}

size_t gl_type_size(GLenum t) {
    return t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 4;
}

void x_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    mixv(mode);
    mixv(first);
    mixv(count);
    for (u32 i = 0; i < 16; ++i) {
        const Attrib& a = g_attribs[i];
        if (!a.enabled || !a.client || count <= 0) continue;
        size_t elem = size_t(a.size) * gl_type_size(a.type);
        size_t stride = a.stride ? size_t(a.stride) : elem;
        const u8* p = a.client + size_t(first) * stride;
        if (stride == elem) {
            mix(p, elem * size_t(count));
        } else {
            for (GLsizei k = 0; k < count; ++k) mix(p + size_t(k) * stride, elem);
        }
    }
    glDrawArrays(mode, first, count);
}

void x_glUniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 16);
    glUniform4fv(loc, n, v);
}
void x_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 64);
    glUniformMatrix4fv(loc, n, t, v);
}
// Uploads always count as a change (hashing whole textures every frame would be wasteful).
void x_glTexImage2D(GLenum t, GLint l, GLint inf, GLsizei w, GLsizei hh, GLint b, GLenum f, GLenum ty, const void* px) {
    mixv(++g_upload_nonce);
    glTexImage2D(t, l, inf, w, hh, b, f, ty, px);
}
void x_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei hh, GLenum f, GLenum ty, const void* px) {
    mixv(++g_upload_nonce);
    glTexSubImage2D(t, l, x, y, w, hh, f, ty, px);
}

void x_glVertexAttribPointer(Ctx& c) {
    GLuint index = c.a32();
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLboolean norm = GLboolean(c.a32());
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    // With a VBO bound the "pointer" is a byte offset into it; otherwise a client-side array.
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : h(ptr);
    if (index < 16) {
        Attrib& a = g_attribs[index];
        a.size = size;
        a.type = type;
        a.stride = stride;
        a.client = t_array_buffer ? nullptr : static_cast<const u8*>(p);
    }
    u32 key[5] = {index, u32(size), type, u32(stride), t_array_buffer ? ptr : 0};
    mix(key, sizeof key);
    glVertexAttribPointer(index, size, type, norm, stride, p);
}

void x_glBufferData(GLenum target, u32 size, const void* data, GLenum usage) {
    mixv(++g_upload_nonce);
    glBufferData(target, GLsizeiptr(size), data, usage);
}

void x_glShaderSource(GLuint shader, GLsizei count, u32 strings, const GLint* lengths) {
    std::vector<const GLchar*> v(count > 0 ? size_t(count) : 0);
    for (GLsizei i = 0; i < count; ++i) v[size_t(i)] = hs(rd<u32>(strings + u32(i) * 4));
    glShaderSource(shader, count, v.data(), lengths);
}

std::mutex g_str_mu;
std::unordered_map<GLenum, gaddr> g_strings;
u32 x_glGetString(GLenum name) {
    const GLubyte* s = glGetString(name);
    if (!s) return 0;
    std::lock_guard lk(g_str_mu);
    auto& slot = g_strings[name];
    if (!slot) slot = gstrdup(reinterpret_cast<const char*>(s));
    return slot;
}

}  // namespace

bool gles_take_frame_changed() {
    static u64 prev = 0;
    bool changed = g_frame_hash != prev;
    prev = g_frame_hash;
    g_frame_hash = 1469598103934665603ull;
    return changed;
}

void register_gles() {
    reg("glActiveTexture", &hashed<&glActiveTexture>);
    NB_REG("glAttachShader", &glAttachShader);
    NB_REG("glBindAttribLocation", &glBindAttribLocation);
    NB_REG("glBindBuffer", x_glBindBuffer);
    reg("glBindFramebuffer", &hashed<&glBindFramebuffer>);
    reg("glBindRenderbuffer", &hashed<&glBindRenderbuffer>);
    reg("glBindTexture", &hashed<&glBindTexture>);
    reg("glBlendFunc", &hashed<&glBlendFunc>);
    reg("glBlendFuncSeparate", &hashed<&glBlendFuncSeparate>);
    NB_REG("glBufferData", x_glBufferData);
    NB_REG("glCheckFramebufferStatus", &glCheckFramebufferStatus);
    reg("glClear", &hashed<&glClear>);
    reg("glClearColor", &hashed<&glClearColor>);
    reg("glClearDepthf", &hashed<&glClearDepthf>);
    reg("glClearStencil", &hashed<&glClearStencil>);
    reg("glColorMask", &hashed<&glColorMask>);
    NB_REG("glCompileShader", &glCompileShader);
    reg("glCopyTexImage2D", &hashed<&glCopyTexImage2D>);
    NB_REG("glCreateProgram", &glCreateProgram);
    NB_REG("glCreateShader", &glCreateShader);
    reg("glCullFace", &hashed<&glCullFace>);
    NB_REG("glDeleteBuffers", &glDeleteBuffers);
    NB_REG("glDeleteFramebuffers", &glDeleteFramebuffers);
    NB_REG("glDeleteProgram", &glDeleteProgram);
    NB_REG("glDeleteShader", &glDeleteShader);
    NB_REG("glDeleteTextures", &glDeleteTextures);
    reg("glDepthFunc", &hashed<&glDepthFunc>);
    reg("glDepthMask", &hashed<&glDepthMask>);
    reg("glDepthRangef", &hashed<&glDepthRangef>);
    reg("glDisable", &hashed<&glDisable>);
    NB_REG("glDisableVertexAttribArray", x_glDisableVertexAttribArray);
    NB_REG("glDrawArrays", x_glDrawArrays);
    reg("glEnable", &hashed<&glEnable>);
    NB_REG("glEnableVertexAttribArray", x_glEnableVertexAttribArray);
    reg("glFramebufferRenderbuffer", &hashed<&glFramebufferRenderbuffer>);
    reg("glFramebufferTexture2D", &hashed<&glFramebufferTexture2D>);
    reg("glFrontFace", &hashed<&glFrontFace>);
    NB_REG("glGenBuffers", &glGenBuffers);
    NB_REG("glGenFramebuffers", &glGenFramebuffers);
    NB_REG("glGenRenderbuffers", &glGenRenderbuffers);
    NB_REG("glGenTextures", &glGenTextures);
    reg("glGenerateMipmap", &hashed<&glGenerateMipmap>);
    NB_REG("glGetActiveAttrib", &glGetActiveAttrib);
    NB_REG("glGetActiveUniform", &glGetActiveUniform);
    NB_REG("glGetAttribLocation", &glGetAttribLocation);
    NB_REG("glGetError", &glGetError);
    NB_REG("glGetIntegerv", &glGetIntegerv);
    NB_REG("glGetProgramInfoLog", &glGetProgramInfoLog);
    NB_REG("glGetProgramiv", &glGetProgramiv);
    NB_REG("glGetShaderInfoLog", &glGetShaderInfoLog);
    NB_REG("glGetShaderiv", &glGetShaderiv);
    NB_REG("glGetString", x_glGetString);
    NB_REG("glGetUniformLocation", &glGetUniformLocation);
    NB_REG("glIsEnabled", &glIsEnabled);
    reg("glLineWidth", &hashed<&glLineWidth>);
    NB_REG("glLinkProgram", &glLinkProgram);
    NB_REG("glReadPixels", &glReadPixels);
    NB_REG("glRenderbufferStorage", &glRenderbufferStorage);
    reg("glScissor", &hashed<&glScissor>);
    NB_REG("glShaderSource", x_glShaderSource);
    reg("glStencilFunc", &hashed<&glStencilFunc>);
    reg("glStencilMask", &hashed<&glStencilMask>);
    reg("glStencilOp", &hashed<&glStencilOp>);
    NB_REG("glTexImage2D", x_glTexImage2D);
    reg("glTexParameteri", &hashed<&glTexParameteri>);
    NB_REG("glTexSubImage2D", x_glTexSubImage2D);
    reg("glUniform1f", &hashed<&glUniform1f>);
    reg("glUniform1i", &hashed<&glUniform1i>);
    reg("glUniform2f", &hashed<&glUniform2f>);
    reg("glUniform3f", &hashed<&glUniform3f>);
    reg("glUniform4f", &hashed<&glUniform4f>);
    NB_REG("glUniform4fv", x_glUniform4fv);
    NB_REG("glUniformMatrix4fv", x_glUniformMatrix4fv);
    reg("glUseProgram", &hashed<&glUseProgram>);
    reg("glVertexAttribPointer", x_glVertexAttribPointer);
    reg("glViewport", &hashed<&glViewport>);
}

}  // namespace nb
