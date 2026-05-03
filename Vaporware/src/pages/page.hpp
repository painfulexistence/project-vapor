#pragma once
#include <RmlUi/Core/ElementDocument.h>

class Page {
public:
    virtual ~Page() = default;

    virtual void onAttach(Rml::ElementDocument* doc, entt::registry& reg) { doc_ = doc; reg_ = &reg; }
    virtual void onDetach() {}
    virtual void onUpdate(float dt) {}

    Rml::ElementDocument* document() const { return doc_; }
    void show() { targetVisible_ = true; }
    void hide() { targetVisible_ = false; }

protected:
    Rml::ElementDocument* doc_ = nullptr;
    entt::registry* reg_ = nullptr;
    bool targetVisible_ = false;

    friend class PageSystem;
};
