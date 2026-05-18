#pragma once

#if defined(_WIN32)
    #define VIRGA_API __declspec(dllexport)
#else
    #define VIRGA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * VirgaNativeAPI.h
 * Standard C API Specification for the Virga Engine Bridge.
 *
 * This file is provided by the Virga.NativeBridge project.
 * Engine implementations must implement these functions to be compatible
 * with the Virga Editor.
 * ========================================================================== */

/* ── Lifecycle ──────────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_Initialize(void);
VIRGA_API void Vapor_Shutdown(void);
VIRGA_API void Vapor_Tick(float deltaTime);
VIRGA_API int  Vapor_IsRunning(void);

/* ── Shared framebuffer (IOSurface / MTLTexture) ─────────────────────────────── */

VIRGA_API void  Vapor_CreateSharedSurface(int width, int height);
VIRGA_API void* Vapor_GetDisplayTexture(void);
VIRGA_API void  Vapor_ReleaseTexture(void* texture);
VIRGA_API int   Vapor_SurfaceWidth(void);
VIRGA_API int   Vapor_SurfaceHeight(void);

/* ── Viewport ───────────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_ResizeView(int width, int height);

/* ── Input ─────────────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_InjectMouseEvent(double x, double y, int button);
VIRGA_API void Vapor_InjectKeyEvent(int sdlScancode, int pressed);

/* ── Scene / mode ──────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_LoadScene(const char* path);
VIRGA_API void Vapor_EnableUIMode(int enable);
VIRGA_API void Vapor_UpdateTerrainSeed(int seed);

/* ── RmlUI ─────────────────────────────────────────────────────────────────────── */

VIRGA_API void        Vapor_Rml_LoadDocument(const char* path);
VIRGA_API void        Vapor_Rml_ReloadDocument(void);
VIRGA_API const char* Vapor_Rml_GetDomTreeJson(void);
VIRGA_API const char* Vapor_Rml_GetElementStyle(const char* elementId);
VIRGA_API void        Vapor_Rml_SetElementStyle(const char* elementId,
                                                const char* property,
                                                const char* value);
VIRGA_API const char* Vapor_Rml_GetElementAt(double x, double y);

#ifdef __cplusplus
}
#endif
