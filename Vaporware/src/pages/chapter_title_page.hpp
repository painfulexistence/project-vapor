#pragma once
#include "page.hpp"
#include <string>

class ChapterTitlePage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
        container_ = doc->GetElementById("chapter-container");
        numberEl_   = doc->GetElementById("chapter-number");
        titleEl_    = doc->GetElementById("chapter-title");
    }

    // Called by ChapterTitleTriggerSystem
    void display(const std::string& number, const std::string& title) {
        if (state_ != State::Hidden) return;
        if (numberEl_) numberEl_->SetInnerRML(number.c_str());
        if (titleEl_)  titleEl_->SetInnerRML(title.c_str());
        state_ = State::FadingIn;
        timer_ = 0.0f;
        if (container_) container_->SetClass("visible", true);
    }

    bool isFullyHidden() const { return state_ == State::Hidden; }

    void onUpdate(float dt) override {
        if (!container_) return;

        switch (state_) {
        case State::Hidden:
            break;
        case State::FadingIn:
            timer_ += dt;
            if (timer_ >= fadeDuration_) {
                state_ = State::Visible;
                timer_ = 0.0f;
            }
            break;
        case State::Visible:
            timer_ += dt;
            if (timer_ >= displayDuration_) {
                state_ = State::FadingOut;
                timer_ = 0.0f;
                container_->SetClass("visible", false);
            }
            break;
        case State::FadingOut:
            timer_ += dt;
            if (timer_ >= fadeDuration_) {
                state_ = State::Hidden;
            }
            break;
        }
    }

    float fadeDuration_    = 0.8f;
    float displayDuration_ = 2.5f;

private:
    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_ = State::Hidden;
    float timer_ = 0.0f;

    Rml::Element* container_ = nullptr;
    Rml::Element* numberEl_  = nullptr;
    Rml::Element* titleEl_   = nullptr;
};
