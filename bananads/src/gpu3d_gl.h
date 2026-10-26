// render.gpu3d: the GLES 3.1 entry points the GPU 3D renderers use, loaded at run time through eglGetProcAddress
// (no link-time dependency on libGLESv2/libEGL: a device without them just keeps Gengis Engine). One translation unit
// defines GPU3D_GL_DEFINE before including this.
#pragma once
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#define GPU3D_GL_FUNCS \
    X(glActiveTexture, PFNGLACTIVETEXTUREPROC) \
    X(glAttachShader, PFNGLATTACHSHADERPROC) \
    X(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC) \
    X(glBindBuffer, PFNGLBINDBUFFERPROC) \
    X(glBindBufferBase, PFNGLBINDBUFFERBASEPROC) \
    X(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
    X(glBindTexture, PFNGLBINDTEXTUREPROC) \
    X(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
    X(glBlendColor, PFNGLBLENDCOLORPROC) \
    X(glBlendEquationSeparate, PFNGLBLENDEQUATIONSEPARATEPROC) \
    X(glBlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC) \
    X(glBufferData, PFNGLBUFFERDATAPROC) \
    X(glBufferSubData, PFNGLBUFFERSUBDATAPROC) \
    X(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
    X(glClear, PFNGLCLEARPROC) \
    X(glClearDepthf, PFNGLCLEARDEPTHFPROC) \
    X(glClientWaitSync, PFNGLCLIENTWAITSYNCPROC) \
    X(glColorMask, PFNGLCOLORMASKPROC) \
    X(glCompileShader, PFNGLCOMPILESHADERPROC) \
    X(glCreateProgram, PFNGLCREATEPROGRAMPROC) \
    X(glCreateShader, PFNGLCREATESHADERPROC) \
    X(glDeleteBuffers, PFNGLDELETEBUFFERSPROC) \
    X(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
    X(glDeleteProgram, PFNGLDELETEPROGRAMPROC) \
    X(glDeleteShader, PFNGLDELETESHADERPROC) \
    X(glDeleteSync, PFNGLDELETESYNCPROC) \
    X(glDeleteTextures, PFNGLDELETETEXTURESPROC) \
    X(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC) \
    X(glDepthFunc, PFNGLDEPTHFUNCPROC) \
    X(glDepthMask, PFNGLDEPTHMASKPROC) \
    X(glDepthRangef, PFNGLDEPTHRANGEFPROC) \
    X(glDisable, PFNGLDISABLEPROC) \
    X(glDrawArrays, PFNGLDRAWARRAYSPROC) \
    X(glDrawBuffers, PFNGLDRAWBUFFERSPROC) \
    X(glDrawElements, PFNGLDRAWELEMENTSPROC) \
    X(glEnable, PFNGLENABLEPROC) \
    X(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    X(glFenceSync, PFNGLFENCESYNCPROC) \
    X(glFlush, PFNGLFLUSHPROC) \
    X(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
    X(glGenBuffers, PFNGLGENBUFFERSPROC) \
    X(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
    X(glGenTextures, PFNGLGENTEXTURESPROC) \
    X(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
    X(glGetError, PFNGLGETERRORPROC) \
    X(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC) \
    X(glGetProgramiv, PFNGLGETPROGRAMIVPROC) \
    X(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC) \
    X(glGetShaderiv, PFNGLGETSHADERIVPROC) \
    X(glGetString, PFNGLGETSTRINGPROC) \
    X(glGetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC) \
    X(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
    X(glLineWidth, PFNGLLINEWIDTHPROC) \
    X(glLinkProgram, PFNGLLINKPROGRAMPROC) \
    X(glReadPixels, PFNGLREADPIXELSPROC) \
    X(glShaderSource, PFNGLSHADERSOURCEPROC) \
    X(glStencilFunc, PFNGLSTENCILFUNCPROC) \
    X(glStencilMask, PFNGLSTENCILMASKPROC) \
    X(glStencilOp, PFNGLSTENCILOPPROC) \
    X(glTexImage2D, PFNGLTEXIMAGE2DPROC) \
    X(glTexParameteri, PFNGLTEXPARAMETERIPROC) \
    X(glTexStorage2D, PFNGLTEXSTORAGE2DPROC) \
    X(glTexStorage3D, PFNGLTEXSTORAGE3DPROC) \
    X(glTexSubImage2D, PFNGLTEXSUBIMAGE2DPROC) \
    X(glTexSubImage3D, PFNGLTEXSUBIMAGE3DPROC) \
    X(glUniform1i, PFNGLUNIFORM1IPROC) \
    X(glUniform1ui, PFNGLUNIFORM1UIPROC) \
    X(glUniform2f, PFNGLUNIFORM2FPROC) \
    X(glUniform4ui, PFNGLUNIFORM4UIPROC) \
    X(glUniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC) \
    X(glUseProgram, PFNGLUSEPROGRAMPROC) \
    X(glVertexAttribIPointer, PFNGLVERTEXATTRIBIPOINTERPROC) \
    X(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \
    X(glViewport, PFNGLVIEWPORTPROC) \
    X(glDispatchCompute, PFNGLDISPATCHCOMPUTEPROC) \
    X(glMemoryBarrier, PFNGLMEMORYBARRIERPROC) \
    X(glBindImageTexture, PFNGLBINDIMAGETEXTUREPROC) \
    X(glMapBufferRange, PFNGLMAPBUFFERRANGEPROC) \
    X(glUnmapBuffer, PFNGLUNMAPBUFFERPROC) \
    X(glGetIntegerv, PFNGLGETINTEGERVPROC) \
    X(glUniform1f, PFNGLUNIFORM1FPROC) \
    X(glUniform2i, PFNGLUNIFORM2IPROC) \
    X(glUniform1iv, PFNGLUNIFORM1IVPROC) \
    X(glBindBufferRange, PFNGLBINDBUFFERRANGEPROC) \
    X(glTexParameterfv, PFNGLTEXPARAMETERFVPROC) \

#define X(n, t) extern t n;
GPU3D_GL_FUNCS
#undef X
// glColorMaski: GLES 3.2, or OES/EXT_draw_buffers_indexed
extern PFNGLCOLORMASKIOESPROC glColorMaski;

// load every entry point with get (eglGetProcAddress); false (logged) when one is missing
bool gpu3d_gl_load(void* (*get)(const char*));

#ifdef GPU3D_GL_DEFINE
#include "Platform.h"
#define X(n, t) t n;
GPU3D_GL_FUNCS
#undef X
PFNGLCOLORMASKIOESPROC glColorMaski;
bool gpu3d_gl_load(void* (*get)(const char*)) {
#define X(n, t) if (!(n = (t)get(#n))) { melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "[gpu3d] missing %s\n", #n); return false; }
    GPU3D_GL_FUNCS
#undef X
    glColorMaski = (PFNGLCOLORMASKIOESPROC)get("glColorMaski");
    if (!glColorMaski) glColorMaski = (PFNGLCOLORMASKIOESPROC)get("glColorMaskiOES");
    if (!glColorMaski) glColorMaski = (PFNGLCOLORMASKIOESPROC)get("glColorMaskiEXT");
    if (!glColorMaski) { melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "[gpu3d] no glColorMaski (GLES 3.2 or OES_draw_buffers_indexed)\n"); return false; }
    return true;
}
#endif
