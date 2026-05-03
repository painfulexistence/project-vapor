#pragma once
#include "page.hpp"
#include <string>

class SubtitlePage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
        container_ = doc->GetElementById("subtitle-container");
        speakerEl_  = doc->GetElementById("subtitle-speaker");
        textEl_     = doc->GetElementById("subtitle-text");
    }

    // Called by SubtitleQueueSystem before show() to populate content
    void setContent(const std::string& speaker, const std::string& text) {
        if (!speakerEl_ || !textEl_) return;
        if (speaker.empty()) {
            speakerEl_->SetClass("hidden", true);
        } else {
            speakerEl_->SetClass("hidden", false);
            speakerEl_->SetInnerRML(speaker.c_str());
        }
        textEl_->SetInnerRML(text.c_str());
    }

    bool isFullyVisible() const { return state_ == State::Visible; }
    bool isFullyHidden()  const { return state_ == State::Hidden; }

    void onUpdate(float dt) override {
        if (!container_) return;

        switch (state_) {
        case State::Hidden:
            if (targetVisible_) {
                state_ = State::FadingIn;
                timer_ = 0.0f;
                container_->SetClass("visible", true);
            }
            break;
        case State::FadingIn:
            timer_ += dt;
            if (!targetVisible_) {
                state_ = State::FadingOut;
                timer_ = 0.0f;
                container_->SetClass("visible", false);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Visible;
            }
            break;
        case State::Visible:
            if (!targetVisible_) {
                state_ = State::FadingOut;
                timer_ = 0.0f;
                container_->SetClass("visible", false);
            }
            break;
        case State::FadingOut:
            timer_ += dt;
            if (targetVisible_) {
                state_ = State::FadingIn;
                timer_ = 0.0f;
                container_->SetClass("visible", true);
            } else if (timer_ >= fadeDuration_) {
                state_ = State::Hidden;
            }
            break;
        }
    }

    float fadeDuration_ = 0.25f;

private:
    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_ = State::Hidden;
    float timer_ = 0.0f;

    Rml::Element* container_ = nullptr;
    Rml::Element* speakerEl_ = nullptr;
    Rml::Element* textEl_    = nullptr;
};
