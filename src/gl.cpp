#include "gl.hpp"

namespace gl {

void (*GenTextures)(GLsizei, GLuint*);
void (*DeleteTextures)(GLsizei, const GLuint*);
void (*BindTexture)(GLenum, GLuint);
void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
void (*TexParameteri)(GLenum, GLenum, GLint);
void (*PixelStorei)(GLenum, GLint);
void (*GenerateMipmap)(GLenum);
void (*GenFramebuffers)(GLsizei, GLuint*);
void (*DeleteFramebuffers)(GLsizei, const GLuint*);
void (*BindFramebuffer)(GLenum, GLuint);
void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
GLenum (*CheckFramebufferStatus)(GLenum);
void (*GetIntegerv)(GLenum, GLint*);
void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
void (*ClearColor)(float, float, float, float);
void (*Clear)(unsigned int);
void (*Enable)(GLenum);
void (*Disable)(GLenum);

// Construit pour x64 uniquement : une seule convention d'appel, pas de __stdcall à gérer.
template <class F> static bool get(ProcLoader l, F& f, const char* name) {
    f = reinterpret_cast<F>(l(name));
    return f != nullptr;
}

bool load(ProcLoader l) {
    bool ok = true;
    ok &= get(l, GenTextures, "glGenTextures");
    ok &= get(l, DeleteTextures, "glDeleteTextures");
    ok &= get(l, BindTexture, "glBindTexture");
    ok &= get(l, TexImage2D, "glTexImage2D");
    ok &= get(l, TexSubImage2D, "glTexSubImage2D");
    ok &= get(l, TexParameteri, "glTexParameteri");
    ok &= get(l, PixelStorei, "glPixelStorei");
    ok &= get(l, GenerateMipmap, "glGenerateMipmap");
    ok &= get(l, GenFramebuffers, "glGenFramebuffers");
    ok &= get(l, DeleteFramebuffers, "glDeleteFramebuffers");
    ok &= get(l, BindFramebuffer, "glBindFramebuffer");
    ok &= get(l, FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= get(l, CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= get(l, GetIntegerv, "glGetIntegerv");
    ok &= get(l, Viewport, "glViewport");
    ok &= get(l, ClearColor, "glClearColor");
    ok &= get(l, Clear, "glClear");
    ok &= get(l, Enable, "glEnable");
    ok &= get(l, Disable, "glDisable");
    return ok;
}

void upload_rgba(GLuint& tex, int& tw, int& th, const uint8_t* px, int w, int h) {
    GLint prev = 0;
    GetIntegerv(TEXTURE_BINDING_2D, &prev);
    if (!tex) {
        GenTextures(1, &tex);
        tw = th = 0;
    }
    BindTexture(TEXTURE_2D, tex);
    PixelStorei(UNPACK_ALIGNMENT, 1);
    if (tw != w || th != h) {
        TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
        TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        TexImage2D(TEXTURE_2D, 0, RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, px);
        tw = w; th = h;
    } else {
        TexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, RGBA, UNSIGNED_BYTE, px);
    }
    GenerateMipmap(TEXTURE_2D);       // réduction propre quand les tuiles sont petites
    BindTexture(TEXTURE_2D, GLuint(prev));
}

} // namespace gl
