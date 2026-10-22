// TubeMaster — les quelques fonctions OpenGL utilisées hors d'ImGui
// (textures des miniatures), chargées à l'exécution.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gl {

using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;

enum : GLenum {
    TEXTURE_2D = 0x0DE1, RGBA = 0x1908, RGB = 0x1907, UNSIGNED_BYTE = 0x1401,
    TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, LINEAR = 0x2601,
    LINEAR_MIPMAP_LINEAR = 0x2703,
    TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F,
    UNPACK_ALIGNMENT = 0x0CF5, RGBA8 = 0x8058,
    FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5,
    FRAMEBUFFER_BINDING = 0x8CA6, TEXTURE_BINDING_2D = 0x8069, VIEWPORT = 0x0BA2,
    COLOR_BUFFER_BIT = 0x4000, BLEND = 0x0BE2,
};

extern void (*GenTextures)(GLsizei, GLuint*);
extern void (*DeleteTextures)(GLsizei, const GLuint*);
extern void (*BindTexture)(GLenum, GLuint);
extern void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
extern void (*TexParameteri)(GLenum, GLenum, GLint);
extern void (*PixelStorei)(GLenum, GLint);
extern void (*GenerateMipmap)(GLenum);
extern void (*GenFramebuffers)(GLsizei, GLuint*);
extern void (*DeleteFramebuffers)(GLsizei, const GLuint*);
extern void (*BindFramebuffer)(GLenum, GLuint);
extern void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum (*CheckFramebufferStatus)(GLenum);
extern void (*GetIntegerv)(GLenum, GLint*);
extern void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
extern void (*ClearColor)(float, float, float, float);
extern void (*Clear)(unsigned int);
extern void (*Enable)(GLenum);
extern void (*Disable)(GLenum);

using ProcLoader = void* (*)(const char*);
bool load(ProcLoader loader);

// Texture RGBA : crée ou met à jour (réutilise l'objet si la taille ne change pas).
void upload_rgba(GLuint& tex, int& tw, int& th, const uint8_t* px, int w, int h);

} // namespace gl
