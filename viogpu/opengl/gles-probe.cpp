// SPDX-License-Identifier: MIT
// Reuse the pinned Mesa raster/readback implementation without calling its
// application-local entrypoint (which loads opengl32.dll unconditionally).
#define main mesa_app_local_probe_main
#include "../../external/mesa/bin/windows-opengl-probe.cpp"
#undef main
#include <EGL/eglext.h>

static void gles_diagnostic(HMODULE egl, HMODULE gl, int version)
{
    auto error = symbol<PFNEGLGETERRORPROC>(egl, "eglGetError");
    auto check = [error](const char *stage, bool ok) {
        const EGLint status = error();
        std::printf("EGL stage=%s success=%d error=0x%04x\n", stage, ok, status);
        std::fflush(stdout);
        if (!ok || status != EGL_SUCCESS)
        {
            std::exit(1);
        }
    };
    auto glError = symbol<PFNGLGETERRORPROC>(gl, "glGetError");
    auto glCheck = [glError](const char *stage) {
        const GLenum status = glError();
        std::printf("GL stage=%s error=0x%04x\n", stage, status);
        std::fflush(stdout);
        if (status != GL_NO_ERROR)
        {
            std::exit(1);
        }
    };
    HWND hwnd = window();
    error();
    EGLDisplay display = symbol<PFNEGLGETDISPLAYPROC>(egl, "eglGetDisplay")(EGL_DEFAULT_DISPLAY);
    check("eglGetDisplay", display != EGL_NO_DISPLAY);
    EGLint major = 0, minor = 0;
    check("eglInitialize", symbol<PFNEGLINITIALIZEPROC>(egl, "eglInitialize")(display, &major, &minor) == EGL_TRUE);
    std::printf("EGL version=%d.%d\n", major, minor);
    check("eglBindAPI", symbol<PFNEGLBINDAPIPROC>(egl, "eglBindAPI")(EGL_OPENGL_ES_API) == EGL_TRUE);
    const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 version == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    const EGLBoolean chosen = symbol<PFNEGLCHOOSECONFIGPROC>(egl, "eglChooseConfig")(display,
                                                                                     attributes,
                                                                                     &config,
                                                                                     1,
                                                                                     &count);
    check("eglChooseConfig", chosen == EGL_TRUE);
    std::printf("EGL config count=%d\n", count);
    if (count != 1)
    {
        std::exit(1);
    }
    auto configAttrib = symbol<PFNEGLGETCONFIGATTRIBPROC>(egl, "eglGetConfigAttrib");
    const EGLint configNames[] = {EGL_CONFIG_ID,
                                  EGL_BUFFER_SIZE,
                                  EGL_RED_SIZE,
                                  EGL_GREEN_SIZE,
                                  EGL_BLUE_SIZE,
                                  EGL_ALPHA_SIZE,
                                  EGL_DEPTH_SIZE,
                                  EGL_STENCIL_SIZE,
                                  EGL_SAMPLE_BUFFERS,
                                  EGL_SAMPLES,
                                  EGL_RENDERABLE_TYPE};
    for (EGLint name : configNames)
    {
        EGLint value = 0;
        check("eglGetConfigAttrib", configAttrib(display, config, name, &value) == EGL_TRUE);
        std::printf("EGL config attribute=0x%04x value=%d\n", name, value);
    }
    const char *extensions = symbol<PFNEGLQUERYSTRINGPROC>(egl, "eglQueryString")(display, EGL_EXTENSIONS);
    check("eglQueryString extensions", extensions != nullptr);
    if (std::strstr(extensions, "EGL_EXT_pixel_format_float"))
    {
        EGLint componentType = 0;
        check("eglGetConfigAttrib component type",
              configAttrib(display, config, EGL_COLOR_COMPONENT_TYPE_EXT, &componentType) == EGL_TRUE);
        std::printf("EGL config component type=0x%04x\n", componentType);
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
    EGLContext context = symbol<PFNEGLCREATECONTEXTPROC>(egl, "eglCreateContext")(display,
                                                                                  config,
                                                                                  EGL_NO_CONTEXT,
                                                                                  contextAttributes);
    check("eglCreateContext", context != EGL_NO_CONTEXT);
    EGLSurface surface = symbol<PFNEGLCREATEWINDOWSURFACEPROC>(egl, "eglCreateWindowSurface")(display,
                                                                                              config,
                                                                                              hwnd,
                                                                                              nullptr);
    check("eglCreateWindowSurface", surface != EGL_NO_SURFACE);
    auto current = symbol<PFNEGLMAKECURRENTPROC>(egl, "eglMakeCurrent");
    check("eglMakeCurrent", current(display, surface, surface, context) == EGL_TRUE);
    renderer(gl);
    glCheck("renderer");
    symbol<PFNGLVIEWPORTPROC>(gl, "glViewport")(0, 0, 64, 64);
    glCheck("glViewport");
    symbol<PFNGLCLEARCOLORPROC>(gl, "glClearColor")(0, 0, 1, 1);
    glCheck("glClearColor");
    symbol<PFNGLCLEARPROC>(gl, "glClear")(GL_COLOR_BUFFER_BIT);
    glCheck("glClear");
    auto getInteger = symbol<PFNGLGETINTEGERVPROC>(gl, "glGetIntegerv");
    const GLenum readNames[] = {GL_IMPLEMENTATION_COLOR_READ_FORMAT,
                                GL_IMPLEMENTATION_COLOR_READ_TYPE,
                                GL_SAMPLE_BUFFERS,
                                GL_SAMPLES};
    for (GLenum name : readNames)
    {
        GLint value = 0;
        getInteger(name, &value);
        glCheck("glGetIntegerv readback state");
        std::printf("GL readback attribute=0x%04x value=0x%04x\n", name, value);
    }
    const GLfloat vertices[] = {-1, -1, 1, -1, 0, 1};
    GLuint program = 0;
    if (version == 2)
    {
        const char *sources[] = {"attribute vec2 pos; void main(){ gl_Position=vec4(pos,0.0,1.0); }",
                                 "precision mediump float; void main(){ gl_FragColor=vec4(1.0,0.0,0.0,1.0); }"};
        auto createShader = symbol<PFNGLCREATESHADERPROC>(gl, "glCreateShader");
        auto shaderSource = symbol<PFNGLSHADERSOURCEPROC>(gl, "glShaderSource");
        auto compileShader = symbol<PFNGLCOMPILESHADERPROC>(gl, "glCompileShader");
        auto shaderiv = symbol<PFNGLGETSHADERIVPROC>(gl, "glGetShaderiv");
        program = symbol<PFNGLCREATEPROGRAMPROC>(gl, "glCreateProgram")();
        glCheck("glCreateProgram");
        for (int i = 0; i < 2; i++)
        {
            GLuint shader = createShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
            glCheck(i == 0 ? "glCreateShader vertex" : "glCreateShader fragment");
            shaderSource(shader, 1, &sources[i], nullptr);
            glCheck("glShaderSource");
            compileShader(shader);
            glCheck("glCompileShader");
            GLint ok = 0;
            shaderiv(shader, GL_COMPILE_STATUS, &ok);
            glCheck("glGetShaderiv");
            if (!ok)
            {
                fail("GLES shader compile");
            }
            symbol<PFNGLATTACHSHADERPROC>(gl, "glAttachShader")(program, shader);
            glCheck("glAttachShader");
            symbol<PFNGLDELETESHADERPROC>(gl, "glDeleteShader")(shader);
            glCheck("glDeleteShader");
        }
        symbol<PFNGLBINDATTRIBLOCATIONPROC>(gl, "glBindAttribLocation")(program, 0, "pos");
        glCheck("glBindAttribLocation");
        symbol<PFNGLLINKPROGRAMPROC>(gl, "glLinkProgram")(program);
        glCheck("glLinkProgram");
        GLint ok = 0;
        symbol<PFNGLGETPROGRAMIVPROC>(gl, "glGetProgramiv")(program, GL_LINK_STATUS, &ok);
        glCheck("glGetProgramiv");
        if (!ok)
        {
            fail("GLES program link");
        }
        symbol<PFNGLUSEPROGRAMPROC>(gl, "glUseProgram")(program);
        glCheck("glUseProgram");
        symbol<PFNGLVERTEXATTRIBPOINTERPROC>(gl, "glVertexAttribPointer")(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glCheck("glVertexAttribPointer client array");
        symbol<PFNGLENABLEVERTEXATTRIBARRAYPROC>(gl, "glEnableVertexAttribArray")(0);
        glCheck("glEnableVertexAttribArray");
    }
    else
    {
        symbol<void(GL_APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat)>(gl, "glColor4f")(1, 0, 0, 1);
        glCheck("glColor4f");
        symbol<void(GL_APIENTRY *)(GLint, GLenum, GLsizei, const void *)>(gl,
                                                                          "glVertexPointer")(2, GL_FLOAT, 0, vertices);
        glCheck("glVertexPointer");
        symbol<void(GL_APIENTRY *)(GLenum)>(gl, "glEnableClientState")(0x8074);
        glCheck("glEnableClientState");
    }
    symbol<void(GL_APIENTRY *)(GLenum, GLint, GLsizei)>(gl, "glDrawArrays")(GL_TRIANGLES, 0, 3);
    glCheck("glDrawArrays");
    symbol<PFNGLFINISHPROC>(gl, "glFinish")();
    glCheck("glFinish");
    GLubyte pixel[4] = {};
    symbol<PFNGLREADPIXELSPROC>(gl, "glReadPixels")(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glCheck("glReadPixels RGBA UNSIGNED_BYTE");
    std::printf("center RGBA=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    if (pixel[0] < 240 || pixel[1] > 15 || pixel[2] > 15)
    {
        fail("rasterized triangle readback mismatch");
    }
    check("eglSwapBuffers", symbol<PFNEGLSWAPBUFFERSPROC>(egl, "eglSwapBuffers")(display, surface) == EGL_TRUE);
    if (program)
    {
        symbol<PFNGLDELETEPROGRAMPROC>(gl, "glDeleteProgram")(program);
    }
    glCheck("glDeleteProgram");
    check("eglReleaseCurrent", current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE);
    check("eglDestroySurface",
          symbol<PFNEGLDESTROYSURFACEPROC>(egl, "eglDestroySurface")(display, surface) == EGL_TRUE);
    check("eglDestroyContext",
          symbol<PFNEGLDESTROYCONTEXTPROC>(egl, "eglDestroyContext")(display, context) == EGL_TRUE);
    check("eglTerminate", symbol<PFNEGLTERMINATEPROC>(egl, "eglTerminate")(display) == EGL_TRUE);
    DestroyWindow(hwnd);
}

int main(int argc, char **argv)
{
    if (argc != 3 ||
        (std::strcmp(argv[1], "--load-only") && std::strcmp(argv[1], "--gles1") && std::strcmp(argv[1], "--gles2")))
    {
        fail("usage: gles-probe --load-only|--gles1|--gles2 <installed payload directory>");
    }
#if defined(_M_ARM64)
    const char *architecture = "arm64";
#elif defined(_M_X64)
    const char *architecture = "x64";
#elif defined(_M_IX86)
    const char *architecture = "x86";
#else
#error Unsupported probe architecture
#endif
    char directory[MAX_PATH];
    const int length = std::snprintf(directory, sizeof(directory), "%s\\%s", argv[2], architecture);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(directory) || !SetCurrentDirectoryA(directory))
    {
        fail("installed architecture directory");
    }

    load(".\\z-1.dll");
    HMODULE vk = load(".\\vulkan-1.dll");
    HMODULE icd = load(".\\vulkan_freedreno.dll");
    load(".\\libgallium_wgl.dll");
    HMODULE egl = load(".\\libEGL.dll");
    HMODULE es1 = load(".\\libGLESv1_CM.dll");
    HMODULE es2 = load(".\\libGLESv2.dll");
    symbol<PROC>(vk, "vkGetInstanceProcAddr");
    symbol<PROC>(icd, "vk_icdGetInstanceProcAddr");
    symbol<PROC>(egl, "eglGetProcAddress");
    symbol<PROC>(es1, "glDrawArrays");
    symbol<PROC>(es2, "glCreateShader");

    if (!std::strcmp(argv[1], "--gles1"))
    {
        gles_diagnostic(egl, es1, 1);
    }
    else if (!std::strcmp(argv[1], "--gles2"))
    {
        gles_diagnostic(egl, es2, 2);
    }
    std::printf("PASS %s architecture=%s (%zu-bit process)\n", argv[1], architecture, sizeof(void *) * 8);
    return 0;
}
