#pragma once
#include "page.hpp"

class HUDPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
        // Start hidden; caller calls show() when ready
        if (auto* el = doc->GetElementById("hud-container"))
            el->SetClass("visible", false);
    }

    bool isFullyVisible() const { return state_ == State::Visible; }
    bool isFullyHidden()  const { return state_ == State::Hidden; }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* el = doc_->GetElementById("hud-container");
        if (!el) return;

        switch (state_) {
        case State::Hidden:
            if (targetVisible_) {
                state_ = State::FadingIn;
                timer_ = 0.0f;
                doc_->Show();
                el->SetClass("visible", true);
            }
            break;
        case State::FadingIn:
            timer_ += dt;
            if (!targetVisible_) {
                state_ = State::FadingOut;
                timer_ = 0.0f;
                el->SetClass("visible", false);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Visible;
            }
            break;
        case State::Visible:
            if (!targetVisible_) {
                state_ = State::FadingOut;
                timer_ = 0.0f;
                el->SetClass("visible", false);
            }
            break;
        case State::FadingOut:
            timer_ += dt;
            if (targetVisible_) {
                state_ = State::FadingIn;
                timer_ = 0.0f;
                el->SetClass("visible", true);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Hidden;
                doc_->Hide();
            }
            break;
        }
    }

private:
    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_       = State::Hidden;
    float timer_       = 0.0f;
    float fadeDuration_ = 0.5f;
};
