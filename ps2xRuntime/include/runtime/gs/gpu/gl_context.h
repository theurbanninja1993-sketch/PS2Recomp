#pragma once

// Headless OpenGL 4.6 core context for the GPU GS renderer (EGL surfaceless,
// usable from any thread, no window), plus the GL entry points it uses.

#include <GL/glcorearb.h>

#include <string>

namespace gsgpu
{
    // X(type, name) for every GL function the renderer calls.
#define GSGPU_GL_FUNCTIONS(X)                                              \
    X(PFNGLGETSTRINGPROC, glGetString)                                     \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                 \
    X(PFNGLGETERRORPROC, glGetError)                                       \
    X(PFNGLENABLEPROC, glEnable)                                           \
    X(PFNGLDISABLEPROC, glDisable)                                         \
    X(PFNGLVIEWPORTPROC, glViewport)                                       \
    X(PFNGLSCISSORPROC, glScissor)                                         \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                   \
    X(PFNGLCLEARPROC, glClear)                                             \
    X(PFNGLFINISHPROC, glFinish)                                           \
    X(PFNGLFLUSHPROC, glFlush)                                             \
    X(PFNGLCOLORMASKPROC, glColorMask)                                     \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                     \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                     \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)             \
    X(PFNGLBLENDCOLORPROC, glBlendColor)                                   \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                 \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                   \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                   \
    X(PFNGLTEXTUREBARRIERPROC, glTextureBarrier)                           \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)               \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                           \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                           \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                       \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                     \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                     \
    X(PFNGLGETTEXTURESUBIMAGEPROC, glGetTextureSubImage)                   \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                         \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)                       \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                   \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                   \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)         \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                         \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer)   \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)               \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                             \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                             \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)                   \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                   \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                           \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)                   \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                   \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                         \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)         \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)         \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)         \
    X(PFNGLVERTEXARRAYATTRIBIFORMATPROC, glVertexArrayAttribIFormat)       \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)       \
    X(PFNGLCREATESHADERPROC, glCreateShader)                               \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                               \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                             \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                 \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                       \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                               \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                             \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                               \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                 \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                               \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                     \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                             \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                   \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                   \
    X(PFNGLPROGRAMUNIFORM1IPROC, glProgramUniform1i)                       \
    X(PFNGLPROGRAMUNIFORM1UIPROC, glProgramUniform1ui)                     \
    X(PFNGLPROGRAMUNIFORM2FPROC, glProgramUniform2f)                       \
    X(PFNGLPROGRAMUNIFORM4FPROC, glProgramUniform4f)                       \
    X(PFNGLPROGRAMUNIFORM4UIPROC, glProgramUniform4ui)

    struct GL
    {
#define GSGPU_DECLARE(type, name) type name = nullptr;
        GSGPU_GL_FUNCTIONS(GSGPU_DECLARE)
#undef GSGPU_DECLARE
    };

    // Owns an EGL display/context. create() makes it current on the calling
    // thread and loads `gl`; all GL use must stay on that thread.
    class GLContext
    {
    public:
        GLContext() = default;
        ~GLContext();
        GLContext(const GLContext &) = delete;
        GLContext &operator=(const GLContext &) = delete;

        bool create(std::string &error);
        void destroy();
        [[nodiscard]] bool valid() const { return m_context != nullptr; }

        GL gl;

    private:
        void *m_display = nullptr; // EGLDisplay
        void *m_context = nullptr; // EGLContext
    };
}
