#pragma once
#include "page.hpp"
#include "page_system.hpp"
#include <RmlUi/Core/EventListener.h>
#include <functional>
#include <memory>
#include <vector>

class PauseMenuPage : public Page {
public:
    explicit PauseMenuPage(std::function<void()> onResume, std::function<void()> onQuitToMenu)
        : onResume_(std::move(onResume)), onQuitToMenu_(std::move(onQuitToMenu)) {}

    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        bind(doc->GetElementById("btn-resume"),       [this] { onResume_(); });
        bind(doc->GetElementById("btn-settings"),     [this, &reg] { PageSystem::push(reg, PageID::Settings); });
        bind(doc->GetElementById("btn-quit-to-menu"), [this] { onQuitToMenu_(); });
    }

    void onDetach() override { listeners_.clear(); }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* el = doc_->GetElementById("pause-menu-container");
        if (!el) return;

        switch (state_) {
        case State::Hidden:
            if (targetVisible_) {
                state_ = State::FadingIn; timer_ = 0.0f;
                doc_->Show();
                el->SetClass("visible", true);
            }
            break;
        case State::FadingIn:
            timer_ += dt;
            if (!targetVisible_) {
                state_ = State::FadingOut; timer_ = 0.0f;
                el->SetClass("visible", false);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Visible;
            }
            break;
        case State::Visible:
            if (!targetVisible_) {
                state_ = State::FadingOut; timer_ = 0.0f;
                el->SetClass("visible", false);
            }
            break;
        case State::FadingOut:
            timer_ += dt;
            if (targetVisible_) {
                state_ = State::FadingIn; timer_ = 0.0f;
                el->SetClass("visible", true);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Hidden;
                doc_->Hide();
            }
            break;
        }
    }

private:
    struct ClickListener : Rml::EventListener {
        std::function<void()> fn;
        void ProcessEvent(Rml::Event&) override { fn(); }
    };

    void bind(Rml::Element* el, std::function<void()> fn) {
        if (!el) return;
        auto l = std::make_unique<ClickListener>();
        l->fn = std::move(fn);
        el->AddEventListener(Rml::EventId::Click, l.get());
        listeners_.emplace_back(el, std::move(l));
    }

    std::vector<std::pair<Rml::Element*, std::unique_ptr<ClickListener>>> listeners_;
    std::function<void()> onResume_;
    std::function<void()> onQuitToMenu_;

    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_        = State::Hidden;
    float timer_        = 0.0f;
    float fadeDuration_ = 0.3f;
};
