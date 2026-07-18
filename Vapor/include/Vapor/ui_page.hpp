#pragma once
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>

namespace Vapor {

class Page {
public:
    Page() = default;
    virtual ~Page() = default;

    // Polymorphic base: copying through a base reference would slice derived
    // page state, so copying is disabled (Core Guidelines C.67).
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    virtual void onAttach(Rml::ElementDocument* doc, entt::registry& reg) { doc_ = doc; reg_ = &reg; }
    virtual void onDetach() {}
    virtual void onUpdate(float dt) {}

    Rml::ElementDocument* document() const { return doc_; }
    virtual void show() { targetVisible_ = true; }
    virtual void hide() { targetVisible_ = false; }

protected:
    Rml::ElementDocument* doc_ = nullptr;
    entt::registry* reg_       = nullptr;
    bool targetVisible_        = false;

    friend class PageSystem;
};

} // namespace Vapor
