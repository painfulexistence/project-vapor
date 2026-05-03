#pragma once
#include "page.hpp"
#include <string>

class ScrollTextPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
        textEl_ = doc->GetElementById("scroll-text");
    }

    // Called by ScrollTextQueueSystem to set the displayed line
    void setLine(const std::string& text) {
        if (textEl_) textEl_->SetInnerRML(text.c_str());
    }

    bool isIdle()         const { return state_ == State::Idle; }
    bool isFullyHidden()  const { return state_ == State::Idle && !targetVisible_; }

    // Called by ScrollTextQueueSystem to trigger a scroll to the next line
    void scrollToNext(const std::string& nextLine) {
        if (state_ != State::Idle) return;
        nextLine_ = nextLine;
        state_ = State::ScrollingOut;
        timer_ = 0.0f;
        if (textEl_) {
            textEl_->SetClass("visible", false);
            textEl_->SetClass("scroll-out", true);
        }
    }

    void onUpdate(float dt) override {
        if (!textEl_) return;

        switch (state_) {
        case State::Idle:
            if (targetVisible_ && !initialised_) {
                initialised_ = true;
                textEl_->SetClass("visible", true);
            }
            break;

        case State::ScrollingOut:
            timer_ += dt;
            if (timer_ >= scrollDuration_) {
                textEl_->SetInnerRML(nextLine_.c_str());
                textEl_->SetClass("scroll-out", false);
                textEl_->SetClass("scroll-in-prepare", true);
                state_ = State::PreparingScrollIn;
                timer_ = 0.0f;
            }
            break;

        case State::PreparingScrollIn:
            // One frame pause so CSS repositions the element before animating
            textEl_->SetClass("scroll-in-prepare", false);
            textEl_->SetClass("scroll-in", true);
            state_ = State::ScrollingIn;
            timer_ = 0.0f;
            break;

        case State::ScrollingIn:
            timer_ += dt;
            if (timer_ >= scrollDuration_) {
                textEl_->SetClass("scroll-in", false);
                textEl_->SetClass("visible", true);
                state_ = State::Idle;
            }
            break;
        }
    }

    float scrollDuration_ = 0.4f;

private:
    enum class State { Idle, ScrollingOut, PreparingScrollIn, ScrollingIn };
    State state_ = State::Idle;
    float timer_ = 0.0f;
    bool  initialised_ = false;
    std::string nextLine_;

    Rml::Element* textEl_ = nullptr;
};
