#pragma once
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>

namespace Vapor {

class Page {
public:
    virtual ~Page() = default;

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
