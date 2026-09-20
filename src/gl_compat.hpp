#pragma once
// Tiny GL loader for symbols we use outside ImGui.
// Call seerrLoadGL() once after glfwMakeContextCurrent.
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

bool seerrLoadGL();

extern void (*seerr_glGenTextures)(GLsizei, GLuint*);
extern void (*seerr_glDeleteTextures)(GLsizei, const GLuint*);
extern void (*seerr_glBindTexture)(GLenum, GLuint);
extern void (*seerr_glTexParameteri)(GLenum, GLenum, GLint);
extern void (*seerr_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern void (*seerr_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
extern void (*seerr_glPixelStorei)(GLenum, GLint);
extern void (*seerr_glViewport)(GLint, GLint, GLsizei, GLsizei);
extern void (*seerr_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*seerr_glClear)(GLbitfield);
extern void (*seerr_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void*);

#define glGenTextures    seerr_glGenTextures
#define glDeleteTextures seerr_glDeleteTextures
#define glBindTexture    seerr_glBindTexture
#define glTexParameteri  seerr_glTexParameteri
#define glTexImage2D     seerr_glTexImage2D
#define glTexSubImage2D  seerr_glTexSubImage2D
#define glPixelStorei    seerr_glPixelStorei
#define glViewport       seerr_glViewport
#define glClearColor     seerr_glClearColor
#define glClear          seerr_glClear
#define glGetTexImage    seerr_glGetTexImage
