#pragma once
#include "page.hpp"
#include "page_system.hpp"
#include <RmlUi/Core/EventListener.h>
#include <functional>
#include <memory>
#include <vector>

class SettingsPage : public Page {
public:
    enum class Tab { Audio, Video, Controls };

    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        bind(doc->GetElementById("tab-audio"),    [this] { switchTab(Tab::Audio); });
        bind(doc->GetElementById("tab-video"),    [this] { switchTab(Tab::Video); });
        bind(doc->GetElementById("tab-controls"), [this] { switchTab(Tab::Controls); });
        bind(doc->GetElementById("btn-back"),     [this, &reg] { PageSystem::pop(reg); });
        switchTab(activeTab_);
    }

    void onDetach() override { listeners_.clear(); }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* el = doc_->GetElementById("settings-container");
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

    Tab activeTab() const { return activeTab_; }

private:
    void switchTab(Tab tab) {
        activeTab_ = tab;
        if (!doc_) return;
        const char* panels[] = { "panel-audio", "panel-video", "panel-controls" };
        const char* tabs[]   = { "tab-audio",   "tab-video",   "tab-controls" };
        for (int i = 0; i < 3; ++i) {
            bool active = (i == static_cast<int>(tab));
            if (auto* el = doc_->GetElementById(panels[i])) el->SetClass("active", active);
            if (auto* el = doc_->GetElementById(tabs[i]))   el->SetClass("active", active);
        }
    }

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
    Tab activeTab_ = Tab::Audio;

    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_        = State::Hidden;
    float timer_        = 0.0f;
    float fadeDuration_ = 0.3f;
};
