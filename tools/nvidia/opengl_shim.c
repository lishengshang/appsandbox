#include "adapter_hooks.h"

#define WGL_FUNCTIONS(X) \
    X(int, wglChoosePixelFormat, (HDC dc, const PIXELFORMATDESCRIPTOR *format), \
      (dc, format), 0) \
    X(BOOL, wglCopyContext, (HGLRC source, HGLRC target, UINT mask), \
      (source, target, mask), FALSE) \
    X(HGLRC, wglCreateContext, (HDC dc), (dc), NULL) \
    X(HGLRC, wglCreateLayerContext, (HDC dc, int layer), (dc, layer), NULL) \
    X(BOOL, wglDeleteContext, (HGLRC context), (context), FALSE) \
    X(BOOL, wglDescribeLayerPlane, \
      (HDC dc, int format, int layer, UINT size, LPLAYERPLANEDESCRIPTOR descriptor), \
      (dc, format, layer, size, descriptor), FALSE) \
    X(int, wglDescribePixelFormat, \
      (HDC dc, int format, UINT size, LPPIXELFORMATDESCRIPTOR descriptor), \
      (dc, format, size, descriptor), 0) \
    X(HGLRC, wglGetCurrentContext, (void), (), NULL) \
    X(HDC, wglGetCurrentDC, (void), (), NULL) \
    X(PROC, wglGetDefaultProcAddress, (LPCSTR name), (name), NULL) \
    X(int, wglGetLayerPaletteEntries, \
      (HDC dc, int layer, int start, int count, COLORREF *colors), \
      (dc, layer, start, count, colors), 0) \
    X(int, wglGetPixelFormat, (HDC dc), (dc), 0) \
    X(PROC, wglGetProcAddress, (LPCSTR name), (name), NULL) \
    X(BOOL, wglMakeCurrent, (HDC dc, HGLRC context), (dc, context), FALSE) \
    X(BOOL, wglRealizeLayerPalette, (HDC dc, int layer, BOOL realize), \
      (dc, layer, realize), FALSE) \
    X(int, wglSetLayerPaletteEntries, \
      (HDC dc, int layer, int start, int count, const COLORREF *colors), \
      (dc, layer, start, count, colors), 0) \
    X(BOOL, wglSetPixelFormat, (HDC dc, int format, const PIXELFORMATDESCRIPTOR *descriptor), \
      (dc, format, descriptor), FALSE) \
    X(BOOL, wglShareLists, (HGLRC source, HGLRC target), (source, target), FALSE) \
    X(BOOL, wglSwapBuffers, (HDC dc), (dc), FALSE) \
    X(BOOL, wglSwapLayerBuffers, (HDC dc, UINT planes), (dc, planes), FALSE) \
    X(DWORD, wglSwapMultipleBuffers, (UINT count, const WGLSWAP *swaps), \
      (count, swaps), 0) \
    X(BOOL, wglUseFontBitmapsA, (HDC dc, DWORD first, DWORD count, DWORD base), \
      (dc, first, count, base), FALSE) \
    X(BOOL, wglUseFontBitmapsW, (HDC dc, DWORD first, DWORD count, DWORD base), \
      (dc, first, count, base), FALSE) \
    X(BOOL, wglUseFontOutlinesA, \
      (HDC dc, DWORD first, DWORD count, DWORD base, FLOAT deviation, \
       FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT metrics), \
      (dc, first, count, base, deviation, extrusion, format, metrics), FALSE) \
    X(BOOL, wglUseFontOutlinesW, \
      (HDC dc, DWORD first, DWORD count, DWORD base, FLOAT deviation, \
       FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT metrics), \
      (dc, first, count, base, deviation, extrusion, format, metrics), FALSE)

#define WGL_DECLARE(result, name, parameters, arguments, failure) \
    typedef result (WINAPI *PFN_##name) parameters; \
    static PFN_##name real_##name;
WGL_FUNCTIONS(WGL_DECLARE)
#undef WGL_DECLARE

static INIT_ONCE s_opengl_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_opengl_backend(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE module;
    (void)once;
    (void)parameter;
    (void)context;

    nvidia_install_adapter_hooks();
    module = LoadLibraryExW(L"appsandbox-opengl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return TRUE;

#define WGL_RESOLVE(result, name, parameters, arguments, failure) \
    real_##name = (PFN_##name)GetProcAddress(module, #name);
    WGL_FUNCTIONS(WGL_RESOLVE)
#undef WGL_RESOLVE
    return TRUE;
}

#define WGL_FORWARD(result, name, parameters, arguments, failure) \
    result WINAPI opengl_##name parameters \
    { \
        InitOnceExecuteOnce(&s_opengl_once, load_opengl_backend, NULL, NULL); \
        if (!real_##name) { \
            SetLastError(ERROR_PROC_NOT_FOUND); \
            return failure; \
        } \
        return real_##name arguments; \
    }
WGL_FUNCTIONS(WGL_FORWARD)
#undef WGL_FORWARD
#undef WGL_FUNCTIONS
