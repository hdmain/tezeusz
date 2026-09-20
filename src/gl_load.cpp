#include "gl_compat.hpp"

#include <GLFW/glfw3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void (*seerr_glGenTextures)(GLsizei, GLuint*) = nullptr;
void (*seerr_glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (*seerr_glBindTexture)(GLenum, GLuint) = nullptr;
void (*seerr_glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*seerr_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (*seerr_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
void (*seerr_glPixelStorei)(GLenum, GLint) = nullptr;
void (*seerr_glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*seerr_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*seerr_glClear)(GLbitfield) = nullptr;
void (*seerr_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void*) = nullptr;

#ifdef _WIN32
static void* loadSym(const char* name) {
    void* p = (void*)glfwGetProcAddress(name);
    if (p) return p;
    static HMODULE opengl = GetModuleHandleA("opengl32.dll");
    if (!opengl) opengl = LoadLibraryA("opengl32.dll");
    return opengl ? (void*)GetProcAddress(opengl, name) : nullptr;
}
#else
static void* loadSym(const char* name) {
    return (void*)glfwGetProcAddress(name);
}
#endif

bool seerrLoadGL() {
    seerr_glGenTextures = (decltype(seerr_glGenTextures))loadSym("glGenTextures");
    seerr_glDeleteTextures = (decltype(seerr_glDeleteTextures))loadSym("glDeleteTextures");
    seerr_glBindTexture = (decltype(seerr_glBindTexture))loadSym("glBindTexture");
    seerr_glTexParameteri = (decltype(seerr_glTexParameteri))loadSym("glTexParameteri");
    seerr_glTexImage2D = (decltype(seerr_glTexImage2D))loadSym("glTexImage2D");
    seerr_glTexSubImage2D = (decltype(seerr_glTexSubImage2D))loadSym("glTexSubImage2D");
    seerr_glPixelStorei = (decltype(seerr_glPixelStorei))loadSym("glPixelStorei");
    seerr_glViewport = (decltype(seerr_glViewport))loadSym("glViewport");
    seerr_glClearColor = (decltype(seerr_glClearColor))loadSym("glClearColor");
    seerr_glClear = (decltype(seerr_glClear))loadSym("glClear");
    seerr_glGetTexImage = (decltype(seerr_glGetTexImage))loadSym("glGetTexImage");

    return seerr_glGenTextures && seerr_glDeleteTextures && seerr_glBindTexture &&
           seerr_glTexParameteri && seerr_glTexImage2D && seerr_glTexSubImage2D &&
           seerr_glPixelStorei && seerr_glViewport && seerr_glClearColor && seerr_glClear;
}
