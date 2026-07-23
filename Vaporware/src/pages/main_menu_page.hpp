#pragma once
#include "page.hpp"
#include "page_system.hpp"
#include <functional>
#include <fmt/core.h>

class MainMenuPage : public Page {
public:
    explicit MainMenuPage(std::function<void()> onStartGame, std::function<void()> onQuit)
        : onStartGame_(std::move(onStartGame)), onQuit_(std::move(onQuit)) {}

    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        bind("btn-start", [this] { onStartGame_(); });
        bind("btn-quit",  [this] { onQuit_(); });
    }

    void onUpdate(float dt) override {
        if (!doc_) return;
        auto* el = doc_->GetElementById("main-menu-container");

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
    std::function<void()> onStartGame_;
    std::function<void()> onQuit_;

    enum class State { Hidden, FadingIn, Visible, FadingOut };
    State state_        = State::Hidden;
    float timer_        = 0.0f;
    float fadeDuration_ = 0.3f;
};
