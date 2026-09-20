#include "gl_compat.hpp"

#include <GLFW/glfw3.h>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
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
// Mirror imgui_impl_opengl3_loader.h: on GLVND systems glfwGetProcAddress alone
// is not always enough; resolve via EGL/GLX + libOpenGL/libGL.
using GlGetProcFn = void* (*)(const char*);

static void* g_libgl = nullptr;
static GlGetProcFn g_getProc = nullptr;
static bool g_tried = false;

static void* tryDlopen(const char* name, int flags) {
    return dlopen(name, flags);
}

static void ensureGlDispatch() {
    if (g_tried) return;
    g_tried = true;

    // Prefer libs already mapped by GLFW / ImGui.
    const int noload = RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD;
    const int load = RTLD_LAZY | RTLD_LOCAL;

    g_libgl = tryDlopen("libOpenGL.so.0", noload);
    if (!g_libgl) g_libgl = tryDlopen("libGL.so.1", noload);
    if (!g_libgl) g_libgl = tryDlopen("libGL.so", noload);
    if (!g_libgl) g_libgl = tryDlopen("libOpenGL.so.0", load);
    if (!g_libgl) g_libgl = tryDlopen("libGL.so.1", load);
    if (!g_libgl) g_libgl = tryDlopen("libGL.so", load);

    void* libegl = tryDlopen("libEGL.so.1", noload);
    if (!libegl) libegl = tryDlopen("libEGL.so.1", load);
    if (libegl) {
        auto egl = (GlGetProcFn)dlsym(libegl, "eglGetProcAddress");
        if (egl) g_getProc = egl;
    }

    if (!g_getProc) {
        void* libglx = tryDlopen("libGLX.so.0", noload);
        if (!libglx) libglx = tryDlopen("libGLX.so.0", load);
        if (libglx) {
            auto glx = (GlGetProcFn)dlsym(libglx, "glXGetProcAddressARB");
            if (!glx) glx = (GlGetProcFn)dlsym(libglx, "glXGetProcAddress");
            if (glx) g_getProc = glx;
        }
    }
}

static void* loadSym(const char* name) {
    if (void* p = (void*)glfwGetProcAddress(name))
        return p;
    ensureGlDispatch();
    if (g_getProc) {
        if (void* p = g_getProc(name))
            return p;
    }
    if (g_libgl) {
        if (void* p = dlsym(g_libgl, name))
            return p;
    }
    return dlsym(RTLD_DEFAULT, name);
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

    const bool ok = seerr_glGenTextures && seerr_glDeleteTextures && seerr_glBindTexture &&
                    seerr_glTexParameteri && seerr_glTexImage2D && seerr_glTexSubImage2D &&
                    seerr_glPixelStorei && seerr_glViewport && seerr_glClearColor && seerr_glClear;
    if (!ok)
        fprintf(stderr, "seerr: OpenGL entry points missing (glGenTextures=%p glClear=%p)\n",
                (void*)seerr_glGenTextures, (void*)seerr_glClear);
    return ok;
}
