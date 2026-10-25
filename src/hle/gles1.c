#include "hle_internal.h"

#ifdef __ANDROID__
#include "gunbros/platform/android_window.h"
#include <GLES/gl.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/glext.h>

static GLuint s_bound_fbo = 0;

static void h_glBindTexture(hle_t *c)       { glBindTexture(c->r0, c->r1); }
static void h_glEnable(hle_t *c)            { glEnable(c->r0); }
static void h_glDisable(hle_t *c)           { glDisable(c->r0); }
static void h_glClear(hle_t *c)             { glClear(c->r0); }
static void h_glClearColor(hle_t *c) {
    float r, g, b, a;
    memcpy(&r, &c->r0, 4); memcpy(&g, &c->r1, 4); memcpy(&b, &c->r2, 4); memcpy(&a, &c->r3, 4);
    glClearColor(r, g, b, a);
}
static void h_glViewport(hle_t *c) {
    int ox = 0, oy = 0;
    window_get_origin(&ox, &oy);
    if (s_bound_fbo == 0) glViewport(c->r0 + ox, c->r1 + oy, c->r2, c->r3);
    else glViewport(c->r0, c->r1, c->r2, c->r3);
}
static void h_glMatrixMode(hle_t *c)        { glMatrixMode(c->r0); }
static void h_glLoadIdentity(hle_t *c)      { (void)c; glLoadIdentity(); }
static void h_glGenTextures(hle_t *c) {
    GLuint *tex = (GLuint*)guest_to_host(c->r1);
    if (tex) glGenTextures(c->r0, tex);
}
static void h_glDeleteTextures(hle_t *c) {
    const GLuint *tex = (const GLuint*)guest_to_host(c->r1);
    if (tex) glDeleteTextures(c->r0, tex);
}
static void h_glTexParameteri(hle_t *c)     { glTexParameteri(c->r0, c->r1, c->r2); }
static void h_glDrawArrays(hle_t *c)        { glDrawArrays(c->r0, c->r1, c->r2); }
static void h_glDrawElements(hle_t *c)      { glDrawElements(c->r0, c->r1, c->r2, guest_ptr(c->r3, 1)); }
static void h_glEnableClientState(hle_t *c) { glEnableClientState(c->r0); }
static void h_glDisableClientState(hle_t *c){ glDisableClientState(c->r0); }
static void h_glVertexPointer(hle_t *c)     { glVertexPointer(c->r0, c->r1, c->r2, guest_ptr(c->r3, 1)); }
static void h_glTexCoordPointer(hle_t *c)   { glTexCoordPointer(c->r0, c->r1, c->r2, guest_ptr(c->r3, 1)); }
static void h_glColorPointer(hle_t *c)      { glColorPointer(c->r0, c->r1, c->r2, guest_ptr(c->r3, 1)); }
static void h_glLoadMatrixf(hle_t *c)       { void *p = guest_ptr(c->r0, 64); if (p) glLoadMatrixf((const GLfloat *)p); }
static void h_glBlendFunc(hle_t *c)         { glBlendFunc(c->r0, c->r1); }
static void h_glCullFace(hle_t *c)          { glCullFace(c->r0); }
static void h_glDepthFunc(hle_t *c)         { glDepthFunc(c->r0); }
static void h_glDepthMask(hle_t *c)         { glDepthMask((GLboolean)c->r0); }
static void h_glDepthRangef(hle_t *c) {
    float n, f; memcpy(&n, &c->r0, 4); memcpy(&f, &c->r1, 4); glDepthRangef(n, f);
}
static void h_glDepthRangex(hle_t *c)       { glDepthRangex(c->r0, c->r1); }
static void h_glClearDepthf(hle_t *c)       { float d; memcpy(&d, &c->r0, 4); glClearDepthf(d); }
static void h_glScissor(hle_t *c) {
    int ox = 0, oy = 0;
    window_get_origin(&ox, &oy);
    if (s_bound_fbo == 0) glScissor(c->r0 + ox, c->r1 + oy, c->r2, c->r3);
    else glScissor(c->r0, c->r1, c->r2, c->r3);
}
static void h_glShadeModel(hle_t *c)        { glShadeModel(c->r0); }
static void h_glAlphaFunc(hle_t *c) {
    float ref; memcpy(&ref, &c->r1, 4); glAlphaFunc(c->r0, ref);
}
static void h_glAlphaFuncx(hle_t *c)        { glAlphaFuncx(c->r0, c->r1); }
static void h_glColor4f(hle_t *c) {
    float r, g, b, a;
    memcpy(&r, &c->r0, 4); memcpy(&g, &c->r1, 4); memcpy(&b, &c->r2, 4); memcpy(&a, &c->r3, 4);
    glColor4f(r, g, b, a);
}
static void h_glColor4x(hle_t *c)           { glColor4x(c->r0, c->r1, c->r2, c->r3); }
static void h_glColorMask(hle_t *c)         { glColorMask(c->r0, c->r1, c->r2, c->r3); }
static void h_glScalex(hle_t *c)            { glScalex(c->r0, c->r1, c->r2); }
static void h_glTexEnvx(hle_t *c)           { glTexEnvx(c->r0, c->r1, c->r2); }
static void h_glActiveTexture(hle_t *c)     { glActiveTexture(c->r0); }
static void h_glClientActiveTexture(hle_t *c){ glClientActiveTexture(c->r0); }
static void h_glFrontFace(hle_t *c)         { glFrontFace(c->r0); }
static void h_glHint(hle_t *c)              { glHint(c->r0, c->r1); }
static void h_glPixelStorei(hle_t *c)       { glPixelStorei(c->r0, c->r1); }
static void h_glPolygonOffset(hle_t *c)     { float f, u; memcpy(&f, &c->r0, 4); memcpy(&u, &c->r1, 4); glPolygonOffset(f, u); }
static void h_glClearStencil(hle_t *c)      { glClearStencil(c->r0); }
static void h_glStencilMask(hle_t *c)       { glStencilMask(c->r0); }
static void h_glGetIntegerv(hle_t *c) {
    GLint *p = (GLint*)guest_to_host(c->r1);
    if (!p) return;
    glGetIntegerv(c->r0, p);
    // The guest keeps the max texture size as an unsigned short, so GPUs reporting
    // 65536 (e.g. Mali) wrap to 0 and CRenderSurface_OGLES_Texture::
    // InitializeTextureObjects divides by zero on every texture upload.
    if (c->r0 == GL_MAX_TEXTURE_SIZE && *p > 2048) *p = 2048;
}
static void h_glGetError(hle_t *c)          { c->r0 = glGetError(); }
static void h_glGetString(hle_t *c) {
    const GLubyte *s = glGetString(c->r0);
    if (s) {
        uint32_t len = (uint32_t)strlen((const char*)s) + 1;
        c->r0 = guest_alloc(len);
        char *dst = (char*)guest_to_host(c->r0);
        if (dst) memcpy(dst, s, len);
    } else {
        c->r0 = 0;
    }
}
static void h_glBindBuffer(hle_t *c)        { glBindBuffer(c->r0, c->r1); }
static void h_glTexImage2D(hle_t *c) {
    uint32_t sp_args[5];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    void *pix = guest_to_host(sp_args[4]);
    glTexImage2D(c->r0, c->r1, c->r2, c->r3, sp_args[0], sp_args[1], sp_args[2], sp_args[3], pix);
}
static void h_glCompressedTexImage2D(hle_t *c) {
    uint32_t sp_args[4];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    void *data = guest_to_host(sp_args[3]);
    glCompressedTexImage2D(c->r0, c->r1, c->r2, c->r3, sp_args[0], sp_args[1], sp_args[2], data);
}
static void h_glCopyTexImage2D(hle_t *c) {
    uint32_t sp_args[4];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    glCopyTexImage2D(c->r0, c->r1, c->r2, c->r3, sp_args[0], sp_args[1], sp_args[2], sp_args[3]);
}
static void h_glReadPixels(hle_t *c) {
    uint32_t sp_args[3];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    void *pixels = guest_to_host(sp_args[2]);
    glReadPixels(c->r0, c->r1, c->r2, c->r3, sp_args[0], sp_args[1], pixels);
}
static void h_glGenRenderbuffersOES(hle_t *c) {
    GLuint *p = (GLuint*)guest_to_host(c->r1);
    if (p) glGenRenderbuffersOES(c->r0, p);
}
static void h_glDeleteRenderbuffersOES(hle_t *c) {
    const GLuint *p = (const GLuint*)guest_to_host(c->r1);
    if (p) glDeleteRenderbuffersOES(c->r0, p);
}
static void h_glBindRenderbufferOES(hle_t *c) {
    glBindRenderbufferOES(c->r0, c->r1);
}
static void h_glRenderbufferStorageOES(hle_t *c) {
    glRenderbufferStorageOES(c->r0, c->r1, c->r2, c->r3);
}
static void h_glGenFramebuffersOES(hle_t *c) {
    GLuint *p = (GLuint*)guest_to_host(c->r1);
    if (p) glGenFramebuffersOES(c->r0, p);
}
static void h_glDeleteFramebuffersOES(hle_t *c) {
    const GLuint *p = (const GLuint*)guest_to_host(c->r1);
    if (p) glDeleteFramebuffersOES(c->r0, p);
}
static void h_glBindFramebufferOES(hle_t *c) {
    s_bound_fbo = c->r1;
    glBindFramebufferOES(c->r0, c->r1);
}
static void h_glFramebufferTexture2DOES(hle_t *c) {
    uint32_t sp_args[1];
    guest_mem_read(c->sp, sp_args, 4);
    glFramebufferTexture2DOES(c->r0, c->r1, c->r2, c->r3, sp_args[0]);
}
static void h_glFramebufferRenderbufferOES(hle_t *c) {
    glFramebufferRenderbufferOES(c->r0, c->r1, c->r2, c->r3);
}
static void h_glCheckFramebufferStatusOES(hle_t *c) {
    c->r0 = glCheckFramebufferStatusOES(c->r0);
}

#else
static void h_gl_stub(hle_t *c) { c->r0 = 0; }
#endif

const hle_def_t g_hle_gles1_defs[] = {
#ifdef __ANDROID__
    {"glBindTexture", h_glBindTexture}, {"glEnable", h_glEnable}, {"glDisable", h_glDisable},
    {"glClear", h_glClear}, {"glClearColor", h_glClearColor}, {"glViewport", h_glViewport},
    {"glMatrixMode", h_glMatrixMode}, {"glLoadIdentity", h_glLoadIdentity},
    {"glGenTextures", h_glGenTextures}, {"glDeleteTextures", h_glDeleteTextures},
    {"glTexParameteri", h_glTexParameteri}, {"glDrawArrays", h_glDrawArrays},
    {"glDrawElements", h_glDrawElements}, {"glEnableClientState", h_glEnableClientState},
    {"glDisableClientState", h_glDisableClientState}, {"glVertexPointer", h_glVertexPointer},
    {"glTexCoordPointer", h_glTexCoordPointer}, {"glColorPointer", h_glColorPointer},
    {"glLoadMatrixf", h_glLoadMatrixf}, {"glBlendFunc", h_glBlendFunc}, {"glCullFace", h_glCullFace},
    {"glDepthFunc", h_glDepthFunc}, {"glDepthMask", h_glDepthMask}, {"glDepthRangef", h_glDepthRangef},
    {"glDepthRangex", h_glDepthRangex}, {"glClearDepthf", h_glClearDepthf}, {"glScissor", h_glScissor},
    {"glShadeModel", h_glShadeModel}, {"glAlphaFunc", h_glAlphaFunc}, {"glAlphaFuncx", h_glAlphaFuncx},
    {"glColor4f", h_glColor4f}, {"glColor4x", h_glColor4x}, {"glColorMask", h_glColorMask},
    {"glScalex", h_glScalex}, {"glTexEnvx", h_glTexEnvx}, {"glActiveTexture", h_glActiveTexture},
    {"glClientActiveTexture", h_glClientActiveTexture}, {"glFrontFace", h_glFrontFace},
    {"glHint", h_glHint}, {"glPixelStorei", h_glPixelStorei}, {"glPolygonOffset", h_glPolygonOffset},
    {"glClearStencil", h_glClearStencil}, {"glStencilMask", h_glStencilMask},
    {"glGetIntegerv", h_glGetIntegerv}, {"glGetError", h_glGetError}, {"glGetString", h_glGetString},
    {"glBindBuffer", h_glBindBuffer}, {"glTexImage2D", h_glTexImage2D},
    {"glCompressedTexImage2D", h_glCompressedTexImage2D}, {"glCopyTexImage2D", h_glCopyTexImage2D},
    {"glReadPixels", h_glReadPixels},
    {"glGenRenderbuffersOES", h_glGenRenderbuffersOES}, {"glDeleteRenderbuffersOES", h_glDeleteRenderbuffersOES},
    {"glBindRenderbufferOES", h_glBindRenderbufferOES}, {"glRenderbufferStorageOES", h_glRenderbufferStorageOES},
    {"glGenFramebuffersOES", h_glGenFramebuffersOES}, {"glDeleteFramebuffersOES", h_glDeleteFramebuffersOES},
    {"glBindFramebufferOES", h_glBindFramebufferOES}, {"glFramebufferTexture2DOES", h_glFramebufferTexture2DOES},
    {"glFramebufferRenderbufferOES", h_glFramebufferRenderbufferOES}, {"glCheckFramebufferStatusOES", h_glCheckFramebufferStatusOES},
#else
    {"glBindTexture", h_gl_stub}, {"glEnable", h_gl_stub}, {"glDisable", h_gl_stub},
    {"glClear", h_gl_stub}, {"glClearColor", h_gl_stub}, {"glViewport", h_gl_stub},
    {"glMatrixMode", h_gl_stub}, {"glLoadIdentity", h_gl_stub},
    {"glGenTextures", h_gl_stub}, {"glDeleteTextures", h_gl_stub},
    {"glTexParameteri", h_gl_stub}, {"glDrawArrays", h_gl_stub},
    {"glDrawElements", h_gl_stub}, {"glEnableClientState", h_gl_stub},
    {"glDisableClientState", h_gl_stub}, {"glVertexPointer", h_gl_stub},
    {"glTexCoordPointer", h_gl_stub}, {"glColorPointer", h_gl_stub},
    {"glLoadMatrixf", h_gl_stub}, {"glBlendFunc", h_gl_stub}, {"glCullFace", h_gl_stub},
    {"glDepthFunc", h_gl_stub}, {"glDepthMask", h_gl_stub}, {"glDepthRangef", h_gl_stub},
    {"glDepthRangex", h_gl_stub}, {"glClearDepthf", h_gl_stub}, {"glScissor", h_gl_stub},
    {"glShadeModel", h_gl_stub}, {"glAlphaFunc", h_gl_stub}, {"glAlphaFuncx", h_gl_stub},
    {"glColor4f", h_gl_stub}, {"glColor4x", h_gl_stub}, {"glColorMask", h_gl_stub},
    {"glScalex", h_gl_stub}, {"glTexEnvx", h_gl_stub}, {"glActiveTexture", h_gl_stub},
    {"glClientActiveTexture", h_gl_stub}, {"glFrontFace", h_gl_stub},
    {"glHint", h_gl_stub}, {"glPixelStorei", h_gl_stub}, {"glPolygonOffset", h_gl_stub},
    {"glClearStencil", h_gl_stub}, {"glStencilMask", h_gl_stub},
    {"glGetIntegerv", h_gl_stub}, {"glGetError", h_gl_stub}, {"glGetString", h_gl_stub},
    {"glBindBuffer", h_gl_stub}, {"glTexImage2D", h_gl_stub},
    {"glCompressedTexImage2D", h_gl_stub}, {"glCopyTexImage2D", h_gl_stub},
    {"glReadPixels", h_gl_stub},
    {"glGenRenderbuffersOES", h_gl_stub}, {"glDeleteRenderbuffersOES", h_gl_stub},
    {"glBindRenderbufferOES", h_gl_stub}, {"glRenderbufferStorageOES", h_gl_stub},
    {"glGenFramebuffersOES", h_gl_stub}, {"glDeleteFramebuffersOES", h_gl_stub},
    {"glBindFramebufferOES", h_gl_stub}, {"glFramebufferTexture2DOES", h_gl_stub},
    {"glFramebufferRenderbufferOES", h_gl_stub}, {"glCheckFramebufferStatusOES", h_gl_stub},
#endif
};

const size_t g_hle_gles1_defs_count = sizeof(g_hle_gles1_defs) / sizeof(g_hle_gles1_defs[0]);
