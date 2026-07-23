#pragma once
#include "page.hpp"
#include "page_system.hpp"
#include <functional>

class SettingsPage : public Page {
public:
    enum class Tab { Audio, Video, Controls };

    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        bind("tab-audio",    [this] { switchTab(Tab::Audio); });
        bind("tab-video",    [this] { switchTab(Tab::Video); });
        bind("tab-controls", [this] { switchTab(Tab::Controls); });
        bind("btn-back",     [this, &reg] { PageSystem::pop(reg); });
        switchTab(activeTab_);
    }

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

    Tab activeTab_ = Tab::Audio;

    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_        = State::Hidden;
    float timer_        = 0.0f;
    float fadeDuration_ = 0.3f;
};
