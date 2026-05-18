#include "VirgaNativeAPI.h"

#include <Vapor/engine_core.hpp>
#include <Vapor/rmlui_manager.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertiesIteratorView.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/Types.h>

#include <SDL3/SDL.h>

#include <string>

// ── Module state ─────────────────────────────────────────────────────────────────────

namespace {

static Vapor::EngineCore* g_engine        = nullptr;
static int                g_surfaceW      = 0;
static int                g_surfaceH      = 0;
// Path as handed to Vapor_Rml_LoadDocument (without SDL_GetBasePath prefix).
static std::string        g_activeDocPath;

// Each JSON-returning function writes into its own stable buffer so that the
// caller may hold the pointer until the next call to the same function.
static std::string g_domTreeBuf;
static std::string g_styleBuf;
static std::string g_elementAtBuf;

// ── Helpers ────────────────────────────────────────────────────────────────────────

static void JsonAppendEscaped(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
}

static void SerializeNode(Rml::Element* elem, std::string& out) {
    out += "{\"tagName\":\"";
    JsonAppendEscaped(out, elem->GetTagName());
    out += "\",\"id\":\"";
    JsonAppendEscaped(out, elem->GetId());
    out += "\",\"className\":\"";
    JsonAppendEscaped(out, elem->GetAttribute<Rml::String>("class", ""));
    out += "\",\"children\":[";
    int n = elem->GetNumChildren();
    for (int i = 0; i < n; ++i) {
        if (i) out += ',';
        SerializeNode(elem->GetChild(i), out);
    }
    out += "]}";
}

static Rml::Context* RmlContext() {
    if (!g_engine) return nullptr;
    auto* rml = g_engine->getRmlUiManager();
    return (rml && rml->IsInitialized()) ? rml->GetContext() : nullptr;
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────────────

extern "C" void Vapor_Initialize(void) {
    if (g_engine) return;
    g_engine = new Vapor::EngineCore();
    g_engine->init();
}

extern "C" void Vapor_Shutdown(void) {
    if (!g_engine) return;
    g_engine->shutdown();
    delete g_engine;
    g_engine = nullptr;
}

extern "C" void Vapor_Tick(float deltaTime) {
    if (g_engine) g_engine->update(deltaTime);
}

extern "C" int Vapor_IsRunning(void) {
    return (g_engine && g_engine->isInitialized()) ? 1 : 0;
}

// ── Shared framebuffer ───────────────────────────────────────────────────────────────

extern "C" void Vapor_CreateSharedSurface(int width, int height) {
    g_surfaceW = width;
    g_surfaceH = height;
    // Kick off partial RmlUI init so that LoadDocument / etc. work once a
    // renderer calls FinalizeInitialization().
    if (g_engine) g_engine->initRmlUI(width, height);
    // TODO: create IOSurface / platform texture and bind to renderer
}

extern "C" void* Vapor_GetDisplayTexture(void) {
    return nullptr; // TODO: return renderer-owned display texture
}

extern "C" void Vapor_ReleaseTexture(void* texture) {
    (void)texture; // TODO: release renderer texture
}

extern "C" int Vapor_SurfaceWidth(void)  { return g_surfaceW; }
extern "C" int Vapor_SurfaceHeight(void) { return g_surfaceH; }

// ── Viewport ──────────────────────────────────────────────────────────────────────

extern "C" void Vapor_ResizeView(int width, int height) {
    g_surfaceW = width;
    g_surfaceH = height;
    if (g_engine) g_engine->onRmlUIResize(width, height);
}

// ── Input ────────────────────────────────────────────────────────────────────────

// button uses SDL conventions: 0 = move only; 1/2/3 = left/middle/right press;
// negative = release (-1/-2/-3 = left/middle/right).
// ProcessMouseButtonDown/Up already map SDL indices to RmlUI indices internally.
extern "C" void Vapor_InjectMouseEvent(double x, double y, int button) {
    if (!g_engine) return;
    auto* rml = g_engine->getRmlUiManager();
    if (!rml || !rml->IsInitialized()) return;
    rml->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
    if (button > 0)      rml->ProcessMouseButtonDown(button, 0);
    else if (button < 0) rml->ProcessMouseButtonUp(-button, 0);
}

extern "C" void Vapor_InjectKeyEvent(int sdlScancode, int pressed) {
    if (!g_engine) return;
    auto* rml = g_engine->getRmlUiManager();
    if (!rml || !rml->IsInitialized()) return;
    auto sc = static_cast<SDL_Scancode>(sdlScancode);
    if (pressed) rml->ProcessKeyDown(sc, 0);
    else         rml->ProcessKeyUp(sc, 0);
}

// ── Scene ────────────────────────────────────────────────────────────────────────

extern "C" void Vapor_LoadScene(const char* path) {
    (void)path;
    // TODO: scene loading API not yet designed
}

// ── RmlUI ───────────────────────────────────────────────────────────────────────

extern "C" void Vapor_Rml_LoadDocument(const char* path) {
    if (!g_engine || !path) return;
    auto* rml = g_engine->getRmlUiManager();
    if (!rml || !rml->IsInitialized()) return;
    g_activeDocPath = path; // remember for ReloadDocument
    rml->LoadDocument(path);
}

extern "C" void Vapor_Rml_ReloadDocument(void) {
    if (!g_engine || g_activeDocPath.empty()) return;
    auto* rml = g_engine->getRmlUiManager();
    if (rml && rml->IsInitialized()) rml->ReloadDocument(g_activeDocPath);
}

extern "C" const char* Vapor_Rml_GetDomTreeJson(void) {
    g_domTreeBuf.clear();
    auto* ctx = RmlContext();
    if (!ctx) { g_domTreeBuf = "[]"; return g_domTreeBuf.c_str(); }

    g_domTreeBuf += '[';
    int n = ctx->GetNumDocuments();
    for (int i = 0; i < n; ++i) {
        if (i) g_domTreeBuf += ',';
        SerializeNode(ctx->GetDocument(i), g_domTreeBuf);
    }
    g_domTreeBuf += ']';
    return g_domTreeBuf.c_str();
}

extern "C" const char* Vapor_Rml_GetElementStyle(const char* elementId) {
    g_styleBuf.clear();
    auto* ctx = RmlContext();
    if (!ctx || !elementId) { g_styleBuf = "{}"; return g_styleBuf.c_str(); }

    Rml::Element* elem = nullptr;
    for (int i = 0, n = ctx->GetNumDocuments(); !elem && i < n; ++i)
        elem = ctx->GetDocument(i)->GetElementById(elementId);
    if (!elem) { g_styleBuf = "{}"; return g_styleBuf.c_str(); }

    g_styleBuf += '{';
    bool first = true;
    for (auto it = elem->IterateLocalProperties(); !it.AtEnd(); ++it) {
        if (!first) g_styleBuf += ',';
        first = false;
        g_styleBuf += '"';
        JsonAppendEscaped(g_styleBuf, Rml::StyleSheetSpecification::GetPropertyName(it.GetName()));
        g_styleBuf += "\":";
        g_styleBuf += '"';
        JsonAppendEscaped(g_styleBuf, it.GetProperty().ToString());
        g_styleBuf += '"';
    }
    g_styleBuf += '}';
    return g_styleBuf.c_str();
}

extern "C" void Vapor_Rml_SetElementStyle(const char* elementId,
                                           const char* property,
                                           const char* value) {
    auto* ctx = RmlContext();
    if (!ctx || !elementId || !property || !value) return;
    for (int i = 0, n = ctx->GetNumDocuments(); i < n; ++i) {
        if (auto* elem = ctx->GetDocument(i)->GetElementById(elementId)) {
            elem->SetProperty(property, value);
            return;
        }
    }
}

extern "C" const char* Vapor_Rml_GetElementAt(double x, double y) {
    g_elementAtBuf.clear();
    auto* ctx = RmlContext();
    if (!ctx) return g_elementAtBuf.c_str();
    Rml::Vector2f pt{static_cast<float>(x), static_cast<float>(y)};
    if (auto* elem = ctx->GetElementAtPoint(pt))
        g_elementAtBuf = elem->GetId();
    return g_elementAtBuf.c_str();
}
