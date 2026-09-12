// SPDX-License-Identifier: MIT
// Actual EGL core/format code and harvested WGL producer functions. No GPU.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/format/u_format.h"
#include "egldisplay.h"
#include "eglconfig.h"
#include "eglarray.h"
#include "eglconfigdebug.h"
#include "eglcurrent.h"
#include "egllog.h"

#ifdef _WIN32
#include "stw_pixelformat.h"
#else
// Linux only: values supplied by the window-system fixture. Windows CI uses
// the actual WGL/SDK type above; this seam does not model or validate its ABI.
#define PFD_DOUBLEBUFFER   1
#define PFD_DRAW_TO_WINDOW 4
#define PFD_TYPE_RGBA      0
struct stw_pixelformat_info
{
    struct
    {
        unsigned dwFlags;
        unsigned char iPixelType, cRedBits, cGreenBits, cBlueBits, cAlphaBits;
        unsigned char cColorBits, cAccumBits, cDepthBits, cStencilBits;
    } pfd;
    struct
    {
        enum pipe_format color_format;
        unsigned samples;
    } stvis;
    bool bindToTextureRGB, bindToTextureRGBA;
};
#endif

struct wgl_egl_config
{
    _EGLConfig base;
    const struct stw_pixelformat_info *stw_config[2];
};

static unsigned checks;
static unsigned failures;
static unsigned egl_errors;

static void expect(bool condition, const char *name)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        printf("FAIL %s\n", name);
    }
}

// Error storage and diagnostic output are the only substituted EGL services.
// Configuration parsing, validation, matching, sorting and ownership are real.
EGLBoolean _eglError(EGLint error, const char *message)
{
    ++egl_errors;
    printf("EGL error 0x%x %s\n", error, message);
    return EGL_FALSE;
}
EGLint _eglGetLogLevel(void)
{
    return _EGL_WARNING;
}
void _eglLog(EGLint level, const char *format, ...)
{
    (void)level;
    (void)format;
}
void eglPrintConfigDebug(const _EGLDisplay *display, const EGLConfig *configs, EGLint count, EGLBoolean chosen)
{
    (void)display;
    (void)configs;
    (void)count;
    (void)chosen;
}

#include "eglarray.c"
#include "eglconfig.c"
// PRODUCTION_FORMATS
// PRODUCTION_WGL

static struct stw_pixelformat_info visual(enum pipe_format format, unsigned bits)
{
    struct stw_pixelformat_info result = {0};
    result.pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    result.pfd.iPixelType = PFD_TYPE_RGBA;
    result.pfd.cRedBits = result.pfd.cGreenBits = result.pfd.cBlueBits = result.pfd.cAlphaBits = (unsigned char)bits;
    result.pfd.cColorBits = (unsigned char)(bits * 4);
    result.stvis.color_format = format;
    return result;
}

static void select_configs(_EGLDisplay *display, EGLint component, unsigned expected_count, enum pipe_format first)
{
    EGLint attributes[] = {EGL_SURFACE_TYPE,
                           EGL_WINDOW_BIT,
                           EGL_RENDERABLE_TYPE,
                           EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE,
                           8,
                           EGL_GREEN_SIZE,
                           8,
                           EGL_BLUE_SIZE,
                           8,
                           EGL_COLOR_COMPONENT_TYPE_EXT,
                           component,
                           EGL_NONE};
    if (component == 0)
    {
        attributes[10] = EGL_NONE; // Exact installed probe's default attribute list.
    }
    EGLConfig selected[8] = {0};
    EGLint count = -1;
    expect(_eglChooseConfig(display, attributes, selected, 8, &count), "real eglChooseConfig succeeds");
    expect(count == (EGLint)expected_count, "float/fixed selection count");
    if (expected_count && count > 0)
    {
        struct wgl_egl_config *config = (struct wgl_egl_config *)selected[0];
        expect(config->stw_config[1]->stvis.color_format == first, "actual selected WGL visual");
        EGLint actual_type = 0;
        expect(_eglGetConfigAttrib(display, &config->base, EGL_COLOR_COMPONENT_TYPE_EXT, &actual_type),
               "real eglGetConfigAttrib succeeds");
        expect(actual_type == (util_format_is_float(first) ? EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT
                                                           : EGL_COLOR_COMPONENT_TYPE_FIXED_EXT),
               "selected EGL type matches actual visual metadata");
    }
}

int main(void)
{
    const unsigned orders[][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (unsigned round = 0; round < 6; ++round)
    {
        _EGLDisplay display = {0};
        display.ClientAPIs = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
        display.Extensions.EXT_pixel_format_float = EGL_TRUE;
        display.Extensions.NOK_texture_from_pixmap = EGL_TRUE;
        struct stw_pixelformat_info visuals[] = {visual(PIPE_FORMAT_R8G8B8A8_UNORM, 8),
                                                 visual(PIPE_FORMAT_R16G16B16A16_FLOAT, 16),
                                                 visual(PIPE_FORMAT_R16G16B16A16_UNORM, 16)};
        // All producer orders must choose RGBA8 from the two actual failure inputs.
        for (unsigned i = 0; i < 2; ++i)
        {
            unsigned index = (round & 1) ? 1 - i : i;
            expect(wgl_add_config(&display, &visuals[index], (int)i + 1, EGL_WINDOW_BIT) != NULL,
                   "real wgl_add_config builds failure inputs");
        }
        select_configs(&display, 0, 1, PIPE_FORMAT_R8G8B8A8_UNORM);
        select_configs(&display, EGL_COLOR_COMPONENT_TYPE_FIXED_EXT, 1, PIPE_FORMAT_R8G8B8A8_UNORM);
        select_configs(&display, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT, 1, PIPE_FORMAT_R16G16B16A16_FLOAT);
        select_configs(&display, EGL_DONT_CARE, 2, PIPE_FORMAT_R16G16B16A16_FLOAT);
        _eglDestroyArray(display.Configs, free);
        display.Configs = NULL;
        // Equal-bit UNORM16 and FLOAT16 must remain distinct, in every order.
        for (unsigned i = 0; i < 3; ++i)
        {
            expect(wgl_add_config(&display, &visuals[orders[round][i]], (int)i + 1, EGL_WINDOW_BIT) != NULL,
                   "float/fixed equal-bit visuals are not merged");
        }
        expect(display.Configs->Size == 3, "three unique actual configs");
        select_configs(&display, 0, 2, PIPE_FORMAT_R16G16B16A16_UNORM);
        select_configs(&display, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT, 1, PIPE_FORMAT_R16G16B16A16_FLOAT);
        select_configs(&display,
                       EGL_DONT_CARE,
                       3,
                       orders[round][0] == 1 || (orders[round][0] == 0 && orders[round][1] == 1) ? PIPE_FORMAT_R16G16B16A16_FLOAT
                                                                                                 : PIPE_FORMAT_R16G16B16A16_UNORM);
        _eglDestroyArray(display.Configs, free);
    }
    expect(egl_errors == 0, "no unexpected EGL error path");
    printf("EGL production config selection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
