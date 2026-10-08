#include "runtime/gs/gpu/gl_context.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace gsgpu
{
    namespace
    {
        EGLDisplay surfacelessDisplay()
        {
            auto getPlatformDisplay =
                reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (getPlatformDisplay)
            {
                EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
                if (display != EGL_NO_DISPLAY)
                    return display;
            }
            return eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
    }

    GLContext::~GLContext()
    {
        destroy();
    }

    bool GLContext::create(std::string &error)
    {
        EGLDisplay display = surfacelessDisplay();
        if (display == EGL_NO_DISPLAY)
        {
            error = "no EGL display";
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(display, &major, &minor))
        {
            error = "eglInitialize failed";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_API))
        {
            error = "eglBindAPI(OpenGL) failed";
            return false;
        }
        const EGLint contextAttributes[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 6,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE};
        // Surfaceless: no config, no surface (EGL_KHR_no_config_context +
        // EGL_KHR_surfaceless_context); rendering goes to framebuffer objects.
        EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttributes);
        if (context == EGL_NO_CONTEXT)
        {
            error = "eglCreateContext (GL 4.6 core) failed";
            return false;
        }
        if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        {
            eglDestroyContext(display, context);
            error = "eglMakeCurrent failed";
            return false;
        }
        m_display = display;
        m_context = context;

#define GSGPU_LOAD(type, name)                                                            \
    gl.name = reinterpret_cast<type>(eglGetProcAddress(#name));                           \
    if (!gl.name)                                                                         \
    {                                                                                     \
        error = "missing GL function " #name;                                             \
        destroy();                                                                        \
        return false;                                                                     \
    }
        GSGPU_GL_FUNCTIONS(GSGPU_LOAD)
#undef GSGPU_LOAD
        return true;
    }

    void GLContext::destroy()
    {
        if (!m_display)
            return;
        auto display = static_cast<EGLDisplay>(m_display);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context)
            eglDestroyContext(display, static_cast<EGLContext>(m_context));
        m_context = nullptr;
        m_display = nullptr;
    }
}
