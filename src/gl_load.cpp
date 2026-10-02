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

void (*tezeusz_glGenTextures)(GLsizei, GLuint*) = nullptr;
void (*tezeusz_glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (*tezeusz_glBindTexture)(GLenum, GLuint) = nullptr;
void (*tezeusz_glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*tezeusz_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (*tezeusz_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
void (*tezeusz_glPixelStorei)(GLenum, GLint) = nullptr;
void (*tezeusz_glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*tezeusz_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*tezeusz_glClear)(GLbitfield) = nullptr;
void (*tezeusz_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void*) = nullptr;
void (*tezeusz_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;

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

bool tezeuszLoadGL() {
    tezeusz_glGenTextures = (decltype(tezeusz_glGenTextures))loadSym("glGenTextures");
    tezeusz_glDeleteTextures = (decltype(tezeusz_glDeleteTextures))loadSym("glDeleteTextures");
    tezeusz_glBindTexture = (decltype(tezeusz_glBindTexture))loadSym("glBindTexture");
    tezeusz_glTexParameteri = (decltype(tezeusz_glTexParameteri))loadSym("glTexParameteri");
    tezeusz_glTexImage2D = (decltype(tezeusz_glTexImage2D))loadSym("glTexImage2D");
    tezeusz_glTexSubImage2D = (decltype(tezeusz_glTexSubImage2D))loadSym("glTexSubImage2D");
    tezeusz_glPixelStorei = (decltype(tezeusz_glPixelStorei))loadSym("glPixelStorei");
    tezeusz_glViewport = (decltype(tezeusz_glViewport))loadSym("glViewport");
    tezeusz_glClearColor = (decltype(tezeusz_glClearColor))loadSym("glClearColor");
    tezeusz_glClear = (decltype(tezeusz_glClear))loadSym("glClear");
    tezeusz_glGetTexImage = (decltype(tezeusz_glGetTexImage))loadSym("glGetTexImage");
    tezeusz_glReadPixels = (decltype(tezeusz_glReadPixels))loadSym("glReadPixels");

    const bool ok = tezeusz_glGenTextures && tezeusz_glDeleteTextures && tezeusz_glBindTexture &&
                    tezeusz_glTexParameteri && tezeusz_glTexImage2D && tezeusz_glTexSubImage2D &&
                    tezeusz_glPixelStorei && tezeusz_glViewport && tezeusz_glClearColor && tezeusz_glClear &&
                    tezeusz_glReadPixels;
    if (!ok)
        fprintf(stderr, "tezeusz: OpenGL entry points missing (glGenTextures=%p glClear=%p)\n",
                (void*)tezeusz_glGenTextures, (void*)tezeusz_glClear);
    return ok;
}
