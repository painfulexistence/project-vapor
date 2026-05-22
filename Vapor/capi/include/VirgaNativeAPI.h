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

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_Initialize(void);
VIRGA_API void Vapor_Shutdown(void);
VIRGA_API void Vapor_Tick(float deltaTime);
VIRGA_API int  Vapor_IsRunning(void);

/* ── Surface ──────────────────────────────────────────────────────────────
 *
 * A Surface is an off-screen render target whose MTLTexture (or equivalent)
 * can be shared with the host editor process via IOSurface.
 *
 * Multiple independent surfaces are supported (e.g. a UI preview panel and
 * a 3D scene viewport can each own a surface).
 *
 * Workflow:
 *   id = Vapor_Surface_Create(w, h)
 *   ...per frame: Vapor_Tick / Vapor_Surface_RenderDocument ...
 *   texture = Vapor_Surface_GetTexture(id)   // hand to Avalonia / SwiftUI
 *   Vapor_Surface_Destroy(id)
 *
 * The returned texture pointer is owned by the surface; callers must NOT
 * free it. It remains valid until Vapor_Surface_Destroy or Vapor_Shutdown.
 * ------------------------------------------------------------------------- */

/* Returns surface ID (>= 1), or -1 on failure. */
VIRGA_API int   Vapor_Surface_Create(int width, int height);
VIRGA_API void  Vapor_Surface_Destroy(int id);
VIRGA_API void  Vapor_Surface_Resize(int id, int width, int height);
/* Returns MTLTexture* (macOS) cast to void*, or NULL if id is invalid. */
VIRGA_API void* Vapor_Surface_GetTexture(int id);
/* Returns 0 if id is invalid. */
VIRGA_API int   Vapor_Surface_Width(int id);
/* Returns 0 if id is invalid. */
VIRGA_API int   Vapor_Surface_Height(int id);

/* ── Input (routed to a specific surface's UI context) ───────────────────── */

VIRGA_API void Vapor_Surface_InjectMouseEvent(int id, double x, double y, int button);
VIRGA_API void Vapor_Surface_InjectKeyEvent(int id, int sdlScancode, int pressed);

/* ── Scene ────────────────────────────────────────────────────────────────── */

VIRGA_API void Vapor_LoadScene(const char* path);

/* ── RmlUI (operates on a specific surface) ──────────────────────────────── */

VIRGA_API void        Vapor_Surface_Rml_LoadDocument(int id, const char* path);
VIRGA_API void        Vapor_Surface_Rml_ReloadDocument(int id);
VIRGA_API const char* Vapor_Surface_Rml_GetDomTreeJson(int id);
VIRGA_API const char* Vapor_Surface_Rml_GetElementStyle(int id, const char* elementId);
VIRGA_API void        Vapor_Surface_Rml_SetElementStyle(int id, const char* elementId,
                                                        const char* property,
                                                        const char* value);
VIRGA_API const char* Vapor_Surface_Rml_GetElementAt(int id, double x, double y);

#ifdef __cplusplus
}
#endif
