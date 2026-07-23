#pragma once
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

// Behavior object for one UI page (button wiring, fade state machines).
// PageSystem drives the lifecycle; the document itself is owned by the Rml
// context and referenced by handle from UIDocumentComponent.
//
// Attach contract (hot reload makes this load-bearing):
//   - onAttach may run MORE THAN ONCE per page object — PageSystem re-attaches
//     after a document reload. Element pointers may be cached only inside
//     onAttach; a re-attach invalidates every previous Rml::Element*.
//   - Overrides of onDetach must call Page::onDetach() — the base tears down
//     bind() listeners while the elements they're attached to are still alive,
//     and forgets the document.
//   - doc_ is valid only between onAttach and onDetach.
class Page {
public:
    Page() = default;
    virtual ~Page() = default;

    // Polymorphic base: copying through a base reference would slice derived
    // page state, so copying is disabled (Core Guidelines C.67).
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    virtual void onAttach(Rml::ElementDocument* doc, entt::registry& reg) { doc_ = doc; reg_ = &reg; }
    virtual void onDetach() { clearBindings(); doc_ = nullptr; }
    virtual void onUpdate(float dt) {}

    Rml::ElementDocument* document() const { return doc_; }
    virtual void show() { targetVisible_ = true; }
    virtual void hide() { targetVisible_ = false; }

protected:
    // Forward an element event to a std::function. The adapter is owned by the
    // page and torn down automatically on detach — pages never hand-roll
    // Rml::EventListener glue. Null element is a no-op (missing ids degrade
    // gracefully, matching GetElementById returning null).
    void bind(Rml::Element* el, std::function<void()> fn, Rml::EventId event = Rml::EventId::Click) {
        if (!el) return;
        auto listener = std::make_unique<BoundListener>(std::move(fn), event);
        el->AddEventListener(event, listener.get());
        bindings_.push_back(std::move(listener));
    }
    void bind(const std::string& elementId, std::function<void()> fn,
              Rml::EventId event = Rml::EventId::Click) {
        bind(doc_ ? doc_->GetElementById(elementId) : nullptr, std::move(fn), event);
    }

    // Detach and free every bind() adapter. Safe in both teardown orders:
    // Rml notifies each adapter via OnDetach when its element dies first, so
    // we only RemoveEventListener from elements that are still alive.
    void clearBindings() {
        for (auto& l : bindings_)
            if (l->element) l->element->RemoveEventListener(l->event, l.get());
        bindings_.clear();
    }

    Rml::ElementDocument* doc_ = nullptr;
    entt::registry* reg_       = nullptr;
    bool targetVisible_        = false;

private:
    class BoundListener : public Rml::EventListener {
    public:
        BoundListener(std::function<void()> fn, Rml::EventId ev) : event(ev), fn_(std::move(fn)) {}
        void ProcessEvent(Rml::Event&) override { if (fn_) fn_(); }
        void OnAttach(Rml::Element* el) override { element = el; }
        void OnDetach(Rml::Element*) override { element = nullptr; }

        Rml::Element* element = nullptr;
        Rml::EventId event;

    private:
        std::function<void()> fn_;
    };

    std::vector<std::unique_ptr<BoundListener>> bindings_;

    friend class PageSystem;
};

} // namespace Vapor
