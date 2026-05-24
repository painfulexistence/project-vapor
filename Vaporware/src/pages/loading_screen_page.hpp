#pragma once
#include "page.hpp"
#include <string>

class LoadingScreenPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        progressEl_ = doc->GetElementById("loading-progress-fill");
        labelEl_     = doc->GetElementById("loading-label");
    }

    // Called each frame by SceneTransitionSystem; value in [0, 1]
    void setProgress(float value) {
        progress_ = value;
        if (!progressEl_) return;
        // Drive width as a percentage via inline property
        std::string width = std::to_string(static_cast<int>(value * 100.0f)) + "%";
        progressEl_->SetProperty("width", width.c_str());
    }

    void setLabel(const std::string& text) {
        if (labelEl_) labelEl_->SetInnerRML(text.c_str());
    }

    bool isFullyVisible() const { return state_ == State::Visible; }
    bool isFullyHidden()  const { return state_ == State::Hidden; }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* el = doc_->GetElementById("loading-container");
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

    float fadeDuration_ = 0.4f;

private:
    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_    = State::Hidden;
    float timer_    = 0.0f;
    float progress_ = 0.0f;

    Rml::Element* progressEl_ = nullptr;
    Rml::Element* labelEl_    = nullptr;
};
