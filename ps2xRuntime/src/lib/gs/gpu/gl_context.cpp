#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // before gl_context.h, which otherwise defines APIENTRY itself
#endif

#include "runtime/gs/gpu/gl_context.h"

#if defined(_WIN32)

namespace gsgpu
{
    namespace
    {
        // WGL_ARB_create_context / _profile
        constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
        constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
        constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
        constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
        using CreateContextAttribsFn = HGLRC(WINAPI *)(HDC, HGLRC, const int *);

        const wchar_t *kWindowClass = L"ps2x-gsgpu";

        // Extension functions come from wglGetProcAddress; the GL 1.1 core
        // ones only from opengl32.dll itself.
        void *glProc(const char *name)
        {
            PROC proc = wglGetProcAddress(name);
            const auto value = reinterpret_cast<intptr_t>(proc);
            if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
            {
                static HMODULE s_opengl = LoadLibraryW(L"opengl32.dll");
                proc = s_opengl ? GetProcAddress(s_opengl, name) : nullptr;
            }
            return reinterpret_cast<void *>(proc);
        }
    }

    GLContext::~GLContext()
    {
        destroy();
    }

    bool GLContext::create(std::string &error)
    {
        // A hidden window only provides a pixel format; rendering goes to
        // framebuffer objects.
        HINSTANCE instance = GetModuleHandleW(nullptr);
        static const bool s_registered = [instance]()
        {
            WNDCLASSW wc{};
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = kWindowClass;
            return RegisterClassW(&wc) != 0;
        }();
        (void)s_registered;
        HWND window = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
        if (!window)
        {
            error = "CreateWindowEx failed";
            return false;
        }
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        const int format = ChoosePixelFormat(dc, &pfd);
        if (format == 0 || !SetPixelFormat(dc, format, &pfd))
        {
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "no OpenGL pixel format";
            return false;
        }
        HGLRC legacy = wglCreateContext(dc);
        if (!legacy || !wglMakeCurrent(dc, legacy))
        {
            if (legacy)
                wglDeleteContext(legacy);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "wglCreateContext failed";
            return false;
        }
        auto createContextAttribs = reinterpret_cast<CreateContextAttribsFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
        const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 6,
                                  WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        HGLRC context = createContextAttribs ? createContextAttribs(dc, nullptr, attributes) : nullptr;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (!context || !wglMakeCurrent(dc, context))
        {
            if (context)
                wglDeleteContext(context);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "wglCreateContextAttribsARB (GL 4.6 core) failed";
            return false;
        }
        m_display = window;
        m_dc = dc;
        m_context = context;

#define GSGPU_LOAD(type, name)                                                            \
    gl.name = reinterpret_cast<type>(glProc(#name));                                      \
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
        wglMakeCurrent(nullptr, nullptr);
        if (m_context)
            wglDeleteContext(static_cast<HGLRC>(m_context));
        if (m_dc)
            ReleaseDC(static_cast<HWND>(m_display), static_cast<HDC>(m_dc));
        DestroyWindow(static_cast<HWND>(m_display));
        m_context = nullptr;
        m_dc = nullptr;
        m_display = nullptr;
    }
}

#else

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

#endif
