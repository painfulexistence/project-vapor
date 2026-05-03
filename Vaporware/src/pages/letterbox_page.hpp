#pragma once
#include "page.hpp"

class LetterboxPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
    }

    bool isOpen()   const { return state_ == State::Open; }
    bool isClosed() const { return state_ == State::Hidden; }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* top    = doc_->GetElementById("letterbox-top");
        auto* bottom = doc_->GetElementById("letterbox-bottom");
        if (!top || !bottom) return;

        switch (state_) {
        case State::Hidden:
            if (targetVisible_) {
                state_ = State::Opening;
                timer_ = 0.0f;
                top->SetClass("open", true);
                bottom->SetClass("open", true);
            }
            break;
        case State::Opening:
            timer_ += dt;
            if (!targetVisible_) {
                state_ = State::Closing;
                timer_ = 0.0f;
                top->SetClass("open", false);
                bottom->SetClass("open", false);
            } else if (timer_ >= animDuration_) {
                state_ = State::Open;
            }
            break;
        case State::Open:
            if (!targetVisible_) {
                state_ = State::Closing;
                timer_ = 0.0f;
                top->SetClass("open", false);
                bottom->SetClass("open", false);
            }
            break;
        case State::Closing:
            timer_ += dt;
            if (targetVisible_) {
                state_ = State::Opening;
                timer_ = 0.0f;
                top->SetClass("open", true);
                bottom->SetClass("open", true);
            } else if (timer_ >= animDuration_) {
                state_ = State::Hidden;
            }
            break;
        }
    }

    float animDuration_ = 0.8f;

private:
    enum class State { Hidden, Opening, Open, Closing };
    State state_ = State::Hidden;
    float timer_ = 0.0f;
};
