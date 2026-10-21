// Recording GLES2 stand-in for headless runs: every call (and the data it consumes) is folded
// into a hash, so two runs can be compared frame by frame.
#include <GLES2/gl2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nb {
extern uint8_t* g_base;
}

namespace {

uint64_t g_hash = 1469598103934665603ull;
uint64_t g_calls = 0, g_draws = 0;
GLuint g_next_id = 1;
GLuint g_array_buffer = 0;

void mix(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        g_hash ^= b[i];
        g_hash *= 1099511628211ull;
    }
}
template <class... T>
void rec(const char* fn, T... args) {
    ++g_calls;
    mix(fn, strlen(fn));
    (mix(&args, sizeof args), ...);
}
// Pointers are hashed as guest addresses so the value is independent of where guest space lives.
uint64_t gptr(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(nb::g_base);
    return v - b < (1ull << 32) ? v - b : v;
}

struct Attrib {
    bool enabled = false;
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    const void* ptr = nullptr;
    GLuint vbo = 0;
};
Attrib g_attribs[16];

size_t type_size(GLenum t) {
    switch (t) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}
size_t pixel_size(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;
    switch (format) {
    case GL_RGBA: return 4;
    case GL_RGB: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}

std::map<std::string, GLint> g_locations;
GLint location(GLuint prog, const GLchar* name) {
    std::string k = std::to_string(prog) + ":" + name;
    auto it = g_locations.find(k);
    if (it != g_locations.end()) return it->second;
    GLint l = GLint(g_locations.size() % 15);
    g_locations[k] = l;
    return l;
}

}  // namespace

extern "C" {

uint64_t fakegl_take_hash(uint64_t* calls, uint64_t* draws) {
    uint64_t h = g_hash;
    *calls = g_calls;
    *draws = g_draws;
    g_hash = 1469598103934665603ull;
    g_calls = g_draws = 0;
    return h;
}

void glActiveTexture(GLenum t) { rec("ActiveTexture", t); }
void glAttachShader(GLuint p, GLuint s) { rec("AttachShader", p, s); }
void glBindAttribLocation(GLuint p, GLuint i, const GLchar* n) {
    rec("BindAttribLocation", p, i);
    mix(n, strlen(n));
}
void glBindBuffer(GLenum t, GLuint b) {
    rec("BindBuffer", t, b);
    if (t == GL_ARRAY_BUFFER) g_array_buffer = b;
}
void glBindFramebuffer(GLenum t, GLuint f) { rec("BindFramebuffer", t, f); }
void glBindRenderbuffer(GLenum t, GLuint r) { rec("BindRenderbuffer", t, r); }
void glBindTexture(GLenum t, GLuint x) { rec("BindTexture", t, x); }
void glBlendFunc(GLenum a, GLenum b) { rec("BlendFunc", a, b); }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { rec("BlendFuncSeparate", a, b, c, d); }
void glBufferData(GLenum t, GLsizeiptr n, const void* d, GLenum u) {
    rec("BufferData", t, int64_t(n), u);
    if (d) mix(d, size_t(n));
}
GLenum glCheckFramebufferStatus(GLenum t) {
    rec("CheckFramebufferStatus", t);
    return GL_FRAMEBUFFER_COMPLETE;
}
void glClear(GLbitfield m) { rec("Clear", m); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { rec("ClearColor", r, g, b, a); }
void glClearDepthf(GLfloat d) { rec("ClearDepthf", d); }
void glClearStencil(GLint s) { rec("ClearStencil", s); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { rec("ColorMask", r, g, b, a); }
void glCompileShader(GLuint s) { rec("CompileShader", s); }
void glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {
    rec("CopyTexImage2D", t, l, f, x, y, w, h, b);
}
GLuint glCreateProgram() {
    rec("CreateProgram");
    return g_next_id++;
}
GLuint glCreateShader(GLenum t) {
    rec("CreateShader", t);
    return g_next_id++;
}
void glCullFace(GLenum m) { rec("CullFace", m); }
void glDeleteBuffers(GLsizei n, const GLuint* b) {
    rec("DeleteBuffers", n);
    mix(b, size_t(n) * 4);
}
void glDeleteFramebuffers(GLsizei n, const GLuint* b) {
    rec("DeleteFramebuffers", n);
    mix(b, size_t(n) * 4);
}
void glDeleteProgram(GLuint p) { rec("DeleteProgram", p); }
void glDeleteShader(GLuint s) { rec("DeleteShader", s); }
void glDeleteTextures(GLsizei n, const GLuint* b) {
    rec("DeleteTextures", n);
    mix(b, size_t(n) * 4);
}
void glDepthFunc(GLenum f) { rec("DepthFunc", f); }
void glDepthMask(GLboolean f) { rec("DepthMask", f); }
void glDepthRangef(GLfloat a, GLfloat b) { rec("DepthRangef", a, b); }
void glDisable(GLenum c) { rec("Disable", c); }
void glDisableVertexAttribArray(GLuint i) {
    rec("DisableVertexAttribArray", i);
    if (i < 16) g_attribs[i].enabled = false;
}
void glDrawArrays(GLenum m, GLint first, GLsizei count) {
    rec("DrawArrays", m, first, count);
    ++g_draws;
    for (auto& a : g_attribs) {
        if (!a.enabled || a.vbo || !a.ptr || count <= 0) continue;
        size_t elem = size_t(a.size) * type_size(a.type);
        size_t stride = a.stride ? size_t(a.stride) : elem;
        auto* base = static_cast<const uint8_t*>(a.ptr) + size_t(first) * stride;
        for (GLsizei i = 0; i < count; ++i) mix(base + size_t(i) * stride, elem);
    }
}
void glEnable(GLenum c) { rec("Enable", c); }
void glEnableVertexAttribArray(GLuint i) {
    rec("EnableVertexAttribArray", i);
    if (i < 16) g_attribs[i].enabled = true;
}
void glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) { rec("FramebufferRenderbuffer", t, a, rt, r); }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint x, GLint l) {
    rec("FramebufferTexture2D", t, a, tt, x, l);
}
void glFrontFace(GLenum m) { rec("FrontFace", m); }
static void gen(const char* fn, GLsizei n, GLuint* out) {
    rec(fn, n);
    for (GLsizei i = 0; i < n; ++i) out[i] = g_next_id++;
}
void glGenBuffers(GLsizei n, GLuint* b) { gen("GenBuffers", n, b); }
void glGenFramebuffers(GLsizei n, GLuint* b) { gen("GenFramebuffers", n, b); }
void glGenRenderbuffers(GLsizei n, GLuint* b) { gen("GenRenderbuffers", n, b); }
void glGenTextures(GLsizei n, GLuint* b) { gen("GenTextures", n, b); }
void glGenerateMipmap(GLenum t) { rec("GenerateMipmap", t); }
void glGetActiveAttrib(GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* size, GLenum* type, GLchar* name) {
    rec("GetActiveAttrib", p, i);
    if (len) *len = 0;
    if (size) *size = 0;
    if (type) *type = 0;
    if (name && bs) *name = 0;
}
void glGetActiveUniform(GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* size, GLenum* type, GLchar* name) {
    rec("GetActiveUniform", p, i);
    if (len) *len = 0;
    if (size) *size = 0;
    if (type) *type = 0;
    if (name && bs) *name = 0;
}
GLint glGetAttribLocation(GLuint p, const GLchar* n) {
    rec("GetAttribLocation", p);
    mix(n, strlen(n));
    return location(p, n);
}
GLenum glGetError() { return GL_NO_ERROR; }
void glGetIntegerv(GLenum p, GLint* d) {
    rec("GetIntegerv", p);
    switch (p) {
    case GL_MAX_TEXTURE_SIZE: *d = 4096; break;
    case GL_VIEWPORT:
        d[0] = d[1] = 0;
        d[2] = 1080;
        d[3] = 760;
        break;
    case GL_MAX_VERTEX_ATTRIBS: *d = 16; break;
    default: *d = 0;
    }
}
void glGetProgramInfoLog(GLuint p, GLsizei, GLsizei* len, GLchar* log) {
    rec("GetProgramInfoLog", p);
    if (len) *len = 0;
    if (log) *log = 0;
}
void glGetProgramiv(GLuint p, GLenum n, GLint* v) {
    rec("GetProgramiv", p, n);
    *v = n == GL_LINK_STATUS || n == GL_VALIDATE_STATUS ? 1 : 0;
}
void glGetShaderInfoLog(GLuint s, GLsizei, GLsizei* len, GLchar* log) {
    rec("GetShaderInfoLog", s);
    if (len) *len = 0;
    if (log) *log = 0;
}
void glGetShaderiv(GLuint s, GLenum n, GLint* v) {
    rec("GetShaderiv", s, n);
    *v = n == GL_COMPILE_STATUS ? 1 : 0;
}
const GLubyte* glGetString(GLenum n) {
    rec("GetString", n);
    switch (n) {
    case GL_VENDOR: return reinterpret_cast<const GLubyte*>("ninbridge");
    case GL_RENDERER: return reinterpret_cast<const GLubyte*>("fakegl");
    case GL_VERSION: return reinterpret_cast<const GLubyte*>("OpenGL ES 2.0 fakegl");
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>("OpenGL ES GLSL ES 1.00");
    default: return reinterpret_cast<const GLubyte*>("");
    }
}
GLint glGetUniformLocation(GLuint p, const GLchar* n) {
    rec("GetUniformLocation", p);
    mix(n, strlen(n));
    return location(p, n);
}
GLboolean glIsEnabled(GLenum c) {
    rec("IsEnabled", c);
    return GL_FALSE;
}
void glLineWidth(GLfloat w) { rec("LineWidth", w); }
void glLinkProgram(GLuint p) { rec("LinkProgram", p); }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* px) {
    rec("ReadPixels", x, y, w, h, f, t);
    memset(px, 0, size_t(w) * size_t(h) * pixel_size(f, t));
}
void glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { rec("RenderbufferStorage", t, f, w, h); }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { rec("Scissor", x, y, w, h); }
void glShaderSource(GLuint s, GLsizei count, const GLchar* const* str, const GLint* len) {
    rec("ShaderSource", s, count);
    for (GLsizei i = 0; i < count; ++i) mix(str[i], len && len[i] >= 0 ? size_t(len[i]) : strlen(str[i]));
}
void glStencilFunc(GLenum f, GLint r, GLuint m) { rec("StencilFunc", f, r, m); }
void glStencilMask(GLuint m) { rec("StencilMask", m); }
void glStencilOp(GLenum a, GLenum b, GLenum c) { rec("StencilOp", a, b, c); }
void glTexImage2D(GLenum t, GLint l, GLint inf, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* px) {
    rec("TexImage2D", t, l, inf, w, h, b, f, ty);
    if (px) mix(px, size_t(w) * size_t(h) * pixel_size(f, ty));
}
void glTexParameteri(GLenum t, GLenum p, GLint v) { rec("TexParameteri", t, p, v); }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void* px) {
    rec("TexSubImage2D", t, l, x, y, w, h, f, ty);
    if (px) mix(px, size_t(w) * size_t(h) * pixel_size(f, ty));
}
void glUniform1f(GLint l, GLfloat a) { rec("Uniform1f", l, a); }
void glUniform1i(GLint l, GLint a) { rec("Uniform1i", l, a); }
void glUniform2f(GLint l, GLfloat a, GLfloat b) { rec("Uniform2f", l, a, b); }
void glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { rec("Uniform3f", l, a, b, c); }
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { rec("Uniform4f", l, a, b, c, d); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat* v) {
    rec("Uniform4fv", l, n);
    mix(v, size_t(n) * 16);
}
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) {
    rec("UniformMatrix4fv", l, n, t);
    mix(v, size_t(n) * 64);
}
void glUseProgram(GLuint p) { rec("UseProgram", p); }
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p) {
    rec("VertexAttribPointer", i, size, type, norm, stride, gptr(p));
    if (i < 16) g_attribs[i] = {g_attribs[i].enabled, size, type, stride, p, g_array_buffer};
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { rec("Viewport", x, y, w, h); }

}  // extern "C"
