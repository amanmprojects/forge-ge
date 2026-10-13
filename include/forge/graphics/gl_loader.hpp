#pragma once
// Minimal OpenGL 3.3 core loader via SDL_GL_GetProcAddress.
// Call forge::gl::load() after context creation. No GLEW/GLAD dependency.
// Usage: forge::gl::glGenBuffers(...), etc.

#include <cstdint>
#include <cstddef>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef long long GLint64;
typedef unsigned long long GLuint64;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef short GLshort;
typedef void GLvoid;
typedef float GLclampf;
typedef unsigned int GLbitfield;

#ifndef GL_TRUE
#define GL_TRUE 1
#define GL_FALSE 0
#endif
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGB 0x190D
#define GL_RED 0x1903
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_POINTS 0x0000
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_FUNC_ADD 0x8006
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_TEXTURE_SIZE 0x0D33

namespace forge::gl {

using ProcLoader = void* (*)(const char*);

#define FORGE_GL_LIST \
    X(glGenVertexArrays, void, (GLsizei n, GLuint* arrays)) \
    X(glBindVertexArray, void, (GLuint array)) \
    X(glDeleteVertexArrays, void, (GLsizei n, const GLuint* arrays)) \
    X(glGenBuffers, void, (GLsizei n, GLuint* buffers)) \
    X(glBindBuffer, void, (GLenum target, GLuint buffer)) \
    X(glBufferData, void, (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(glBufferSubData, void, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(glDeleteBuffers, void, (GLsizei n, const GLuint* buffers)) \
    X(glEnableVertexAttribArray, void, (GLuint index)) \
    X(glVertexAttribPointer, void, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
    X(glCreateShader, GLuint, (GLenum type)) \
    X(glShaderSource, void, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)) \
    X(glCompileShader, void, (GLuint shader)) \
    X(glGetShaderiv, void, (GLuint shader, GLenum pname, GLint* params)) \
    X(glGetShaderInfoLog, void, (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(glDeleteShader, void, (GLuint shader)) \
    X(glCreateProgram, GLuint, (void)) \
    X(glAttachShader, void, (GLuint program, GLuint shader)) \
    X(glLinkProgram, void, (GLuint program)) \
    X(glGetProgramiv, void, (GLuint program, GLenum pname, GLint* params)) \
    X(glGetProgramInfoLog, void, (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(glDeleteProgram, void, (GLuint program)) \
    X(glUseProgram, void, (GLuint program)) \
    X(glDetachShader, void, (GLuint program, GLuint shader)) \
    X(glGetUniformLocation, GLint, (GLuint program, const GLchar* name)) \
    X(glUniform1i, void, (GLint location, GLint v0)) \
    X(glUniform1iv, void, (GLint location, GLsizei count, const GLint* value)) \
    X(glUniform1f, void, (GLint location, GLfloat v0)) \
    X(glUniform2f, void, (GLint location, GLfloat v0, GLfloat v1)) \
    X(glUniform3f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(glUniform4f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(glUniformMatrix4fv, void, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(glGenTextures, void, (GLsizei n, GLuint* textures)) \
    X(glBindTexture, void, (GLenum target, GLuint texture)) \
    X(glTexImage2D, void, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)) \
    X(glTexSubImage2D, void, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)) \
    X(glTexParameteri, void, (GLenum target, GLenum pname, GLint param)) \
    X(glDeleteTextures, void, (GLsizei n, const GLuint* textures)) \
    X(glActiveTexture, void, (GLenum texture)) \
    X(glGenerateMipmap, void, (GLenum target)) \
    X(glEnable, void, (GLenum cap)) \
    X(glDisable, void, (GLenum cap)) \
    X(glBlendFunc, void, (GLenum sfactor, GLenum dfactor)) \
    X(glBlendFuncSeparate, void, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)) \
    X(glBlendEquation, void, (GLenum mode)) \
    X(glViewport, void, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(glClearColor, void, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(glClear, void, (GLbitfield mask)) \
    X(glDrawElements, void, (GLenum mode, GLsizei count, GLenum type, const void* indices)) \
    X(glDrawArrays, void, (GLenum mode, GLint first, GLsizei count)) \
    X(glGetError, GLenum, (void)) \
    X(glGetString, const GLubyte*, (GLenum name)) \
    X(glPixelStorei, void, (GLenum pname, GLint param)) \
    X(glGenFramebuffers, void, (GLsizei n, GLuint* ids)) \
    X(glBindFramebuffer, void, (GLenum target, GLuint framebuffer)) \
    X(glFramebufferTexture2D, void, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(glCheckFramebufferStatus, GLenum, (GLenum target)) \
    X(glDeleteFramebuffers, void, (GLsizei n, const GLuint* framebuffers)) \
    X(glScissor, void, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(glGetIntegerv, void, (GLenum pname, GLint* data)) \
    X(glReadPixels, void, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels))

#define X(name, ret, args) using name##Proc = ret (*) args; extern name##Proc name;
FORGE_GL_LIST
#undef X

bool load(ProcLoader loader);
bool isLoaded();

} // namespace forge::gl
