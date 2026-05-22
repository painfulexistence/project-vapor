#include "VirgaNativeAPI.h"

#include <Vapor/engine_core.hpp>
#include <Vapor/ui_renderer.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/PropertiesIteratorView.h>
#include <RmlUi/Core/Types.h>

#include <memory>
#include <string>
#include <unordered_map>

// ── Module state ──────────────────────────────────────────────────────────────

namespace {

struct Surface {
    std::unique_ptr<Vapor::UIRenderer> renderer;
    int width  = 0;
    int height = 0;
};

static Vapor::EngineCore*               g_engine  = nullptr;
static std::unordered_map<int, Surface> g_surfaces;
static int                              g_nextId  = 1;

// Per-call scratch buffers (returned as const char*)
static std::string g_domTreeBuf;
static std::string g_styleBuf;
static std::string g_elementAtBuf;

// ── Helpers ───────────────────────────────────────────────────────────────────

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

static Vapor::UIRenderer* GetRenderer(int id) {
    auto it = g_surfaces.find(id);
    return (it != g_surfaces.end()) ? it->second.renderer.get() : nullptr;
}

static Rml::Context* GetContext(int id) {
    auto* r = GetRenderer(id);
    return r ? r->getContext() : nullptr;
}

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

extern "C" void Vapor_Initialize(void) {
    if (g_engine) return;
    g_engine = new Vapor::EngineCore();
    g_engine->init();
}

extern "C" void Vapor_Shutdown(void) {
    // Destroy all surfaces first (triggers Rml::Shutdown on the last one)
    for (auto& [id, surf] : g_surfaces)
        surf.renderer->shutdown();
    g_surfaces.clear();

    if (!g_engine) return;
    g_engine->shutdown();
    delete g_engine;
    g_engine = nullptr;
}

extern "C" void Vapor_Tick(float deltaTime) {
    if (g_engine) g_engine->update(deltaTime);
    for (auto& [id, surf] : g_surfaces)
        surf.renderer->renderFrame();
}

extern "C" int Vapor_IsRunning(void) {
    return (g_engine && g_engine->isInitialized()) ? 1 : 0;
}

// ── Surface ───────────────────────────────────────────────────────────────────

extern "C" int Vapor_Surface_Create(int width, int height) {
    auto renderer = Vapor::UIRenderer::create(width, height);
    if (!renderer) return -1;
    int id = g_nextId++;
    g_surfaces[id] = { std::move(renderer), width, height };
    return id;
}

extern "C" void Vapor_Surface_Destroy(int id) {
    auto it = g_surfaces.find(id);
    if (it == g_surfaces.end()) return;
    it->second.renderer->shutdown();
    g_surfaces.erase(it);
}

extern "C" void Vapor_Surface_Resize(int id, int width, int height) {
    auto it = g_surfaces.find(id);
    if (it == g_surfaces.end()) return;
    it->second.renderer->resize(width, height);
    it->second.width  = width;
    it->second.height = height;
}

extern "C" void* Vapor_Surface_GetTexture(int id) {
    auto* r = GetRenderer(id);
    return r ? r->getSharedTexture() : nullptr;
}

extern "C" int Vapor_Surface_Width(int id) {
    auto it = g_surfaces.find(id);
    return (it != g_surfaces.end()) ? it->second.width : 0;
}

extern "C" int Vapor_Surface_Height(int id) {
    auto it = g_surfaces.find(id);
    return (it != g_surfaces.end()) ? it->second.height : 0;
}

// ── Input ─────────────────────────────────────────────────────────────────────

extern "C" void Vapor_Surface_InjectMouseEvent(int id, double x, double y, int button) {
    if (auto* r = GetRenderer(id)) r->injectMouseEvent(x, y, button);
}

extern "C" void Vapor_Surface_InjectKeyEvent(int id, int sdlScancode, int pressed) {
    if (auto* r = GetRenderer(id)) r->injectKeyEvent(sdlScancode, pressed);
}

// ── Scene ─────────────────────────────────────────────────────────────────────

extern "C" void Vapor_LoadScene(const char* path) {
    (void)path;
    // TODO: scene loading API not yet designed
}

// ── RmlUI ─────────────────────────────────────────────────────────────────────

extern "C" void Vapor_Surface_Rml_LoadDocument(int id, const char* path) {
    if (auto* r = GetRenderer(id); r && path) r->loadDocument(path);
}

extern "C" void Vapor_Surface_Rml_ReloadDocument(int id) {
    if (auto* r = GetRenderer(id)) r->reloadDocument();
}

extern "C" const char* Vapor_Surface_Rml_GetDomTreeJson(int id) {
    g_domTreeBuf.clear();
    auto* ctx = GetContext(id);
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

extern "C" const char* Vapor_Surface_Rml_GetElementStyle(int id, const char* elementId) {
    g_styleBuf.clear();
    auto* ctx = GetContext(id);
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
        JsonAppendEscaped(g_styleBuf, it.GetName());
        g_styleBuf += "\":\"";
        JsonAppendEscaped(g_styleBuf, it.GetProperty().ToString());
        g_styleBuf += '"';
    }
    g_styleBuf += '}';
    return g_styleBuf.c_str();
}

extern "C" void Vapor_Surface_Rml_SetElementStyle(int id, const char* elementId,
                                                   const char* property,
                                                   const char* value) {
    auto* ctx = GetContext(id);
    if (!ctx || !elementId || !property || !value) return;
    for (int i = 0, n = ctx->GetNumDocuments(); i < n; ++i) {
        if (auto* elem = ctx->GetDocument(i)->GetElementById(elementId)) {
            elem->SetProperty(property, value);
            return;
        }
    }
}

extern "C" const char* Vapor_Surface_Rml_GetElementAt(int id, double x, double y) {
    g_elementAtBuf.clear();
    auto* ctx = GetContext(id);
    if (!ctx) return g_elementAtBuf.c_str();
    Rml::Vector2f pt{static_cast<float>(x), static_cast<float>(y)};
    if (auto* elem = ctx->GetElementAtPoint(pt))
        g_elementAtBuf = elem->GetId();
    return g_elementAtBuf.c_str();
}
