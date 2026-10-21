// OpenGL ES 2.0 imports. All prototypes use 32-bit scalar types, so most are bound directly with
// automatic pointer translation; the few with host-width or pointer-array parameters are wrapped.
#include <GLES2/gl2.h>

#include <mutex>
#include <unordered_map>

#include "nb.h"

namespace nb {
namespace {

thread_local GLuint t_array_buffer = 0;

void x_glBindBuffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) t_array_buffer = buf;
    glBindBuffer(target, buf);
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
    glVertexAttribPointer(index, size, type, norm, stride, p);
}

void x_glBufferData(GLenum target, u32 size, const void* data, GLenum usage) {
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

void register_gles() {
    NB_REG("glActiveTexture", &glActiveTexture);
    NB_REG("glAttachShader", &glAttachShader);
    NB_REG("glBindAttribLocation", &glBindAttribLocation);
    NB_REG("glBindBuffer", x_glBindBuffer);
    NB_REG("glBindFramebuffer", &glBindFramebuffer);
    NB_REG("glBindRenderbuffer", &glBindRenderbuffer);
    NB_REG("glBindTexture", &glBindTexture);
    NB_REG("glBlendFunc", &glBlendFunc);
    NB_REG("glBlendFuncSeparate", &glBlendFuncSeparate);
    NB_REG("glBufferData", x_glBufferData);
    NB_REG("glCheckFramebufferStatus", &glCheckFramebufferStatus);
    NB_REG("glClear", &glClear);
    NB_REG("glClearColor", &glClearColor);
    NB_REG("glClearDepthf", &glClearDepthf);
    NB_REG("glClearStencil", &glClearStencil);
    NB_REG("glColorMask", &glColorMask);
    NB_REG("glCompileShader", &glCompileShader);
    NB_REG("glCopyTexImage2D", &glCopyTexImage2D);
    NB_REG("glCreateProgram", &glCreateProgram);
    NB_REG("glCreateShader", &glCreateShader);
    NB_REG("glCullFace", &glCullFace);
    NB_REG("glDeleteBuffers", &glDeleteBuffers);
    NB_REG("glDeleteFramebuffers", &glDeleteFramebuffers);
    NB_REG("glDeleteProgram", &glDeleteProgram);
    NB_REG("glDeleteShader", &glDeleteShader);
    NB_REG("glDeleteTextures", &glDeleteTextures);
    NB_REG("glDepthFunc", &glDepthFunc);
    NB_REG("glDepthMask", &glDepthMask);
    NB_REG("glDepthRangef", &glDepthRangef);
    NB_REG("glDisable", &glDisable);
    NB_REG("glDisableVertexAttribArray", &glDisableVertexAttribArray);
    NB_REG("glDrawArrays", &glDrawArrays);
    NB_REG("glEnable", &glEnable);
    NB_REG("glEnableVertexAttribArray", &glEnableVertexAttribArray);
    NB_REG("glFramebufferRenderbuffer", &glFramebufferRenderbuffer);
    NB_REG("glFramebufferTexture2D", &glFramebufferTexture2D);
    NB_REG("glFrontFace", &glFrontFace);
    NB_REG("glGenBuffers", &glGenBuffers);
    NB_REG("glGenFramebuffers", &glGenFramebuffers);
    NB_REG("glGenRenderbuffers", &glGenRenderbuffers);
    NB_REG("glGenTextures", &glGenTextures);
    NB_REG("glGenerateMipmap", &glGenerateMipmap);
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
    NB_REG("glLineWidth", &glLineWidth);
    NB_REG("glLinkProgram", &glLinkProgram);
    NB_REG("glReadPixels", &glReadPixels);
    NB_REG("glRenderbufferStorage", &glRenderbufferStorage);
    NB_REG("glScissor", &glScissor);
    NB_REG("glShaderSource", x_glShaderSource);
    NB_REG("glStencilFunc", &glStencilFunc);
    NB_REG("glStencilMask", &glStencilMask);
    NB_REG("glStencilOp", &glStencilOp);
    NB_REG("glTexImage2D", &glTexImage2D);
    NB_REG("glTexParameteri", &glTexParameteri);
    NB_REG("glTexSubImage2D", &glTexSubImage2D);
    NB_REG("glUniform1f", &glUniform1f);
    NB_REG("glUniform1i", &glUniform1i);
    NB_REG("glUniform2f", &glUniform2f);
    NB_REG("glUniform3f", &glUniform3f);
    NB_REG("glUniform4f", &glUniform4f);
    NB_REG("glUniform4fv", &glUniform4fv);
    NB_REG("glUniformMatrix4fv", &glUniformMatrix4fv);
    NB_REG("glUseProgram", &glUseProgram);
    reg("glVertexAttribPointer", x_glVertexAttribPointer);
    NB_REG("glViewport", &glViewport);
}

}  // namespace nb
