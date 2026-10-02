#pragma once
// Tiny GL loader for symbols we use outside ImGui.
// Call tezeuszLoadGL() once after glfwMakeContextCurrent.
// Build with GLFW_INCLUDE_NONE so GLFW does not pull system GL headers.

#include <stddef.h>
#include <stdint.h>

using GLenum = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif

bool tezeuszLoadGL();

extern void (*tezeusz_glGenTextures)(GLsizei, GLuint*);
extern void (*tezeusz_glDeleteTextures)(GLsizei, const GLuint*);
extern void (*tezeusz_glBindTexture)(GLenum, GLuint);
extern void (*tezeusz_glTexParameteri)(GLenum, GLenum, GLint);
extern void (*tezeusz_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern void (*tezeusz_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
extern void (*tezeusz_glPixelStorei)(GLenum, GLint);
extern void (*tezeusz_glViewport)(GLint, GLint, GLsizei, GLsizei);
extern void (*tezeusz_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*tezeusz_glClear)(GLbitfield);
extern void (*tezeusz_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void*);
extern void (*tezeusz_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

#define glGenTextures    tezeusz_glGenTextures
#define glDeleteTextures tezeusz_glDeleteTextures
#define glBindTexture    tezeusz_glBindTexture
#define glTexParameteri  tezeusz_glTexParameteri
#define glTexImage2D     tezeusz_glTexImage2D
#define glTexSubImage2D  tezeusz_glTexSubImage2D
#define glPixelStorei    tezeusz_glPixelStorei
#define glViewport       tezeusz_glViewport
#define glClearColor     tezeusz_glClearColor
#define glClear          tezeusz_glClear
#define glGetTexImage    tezeusz_glGetTexImage
#define glReadPixels     tezeusz_glReadPixels
