#pragma once
#include "page.hpp"
#include <string>

class SelectionOverlayPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        doc->Show();
        panel_  = doc->GetElementById("selection-panel");
        nameEl_ = doc->GetElementById("selection-name");
    }

    void showAt(float screenX, float screenY, const std::string& name) {
        if (!panel_) return;
        // Center the panel horizontally on the projected point, above it
        float px = screenX - 80.0f;
        float py = screenY - 90.0f;
        panel_->SetProperty("left", std::to_string(static_cast<int>(px)) + "px");
        panel_->SetProperty("top",  std::to_string(static_cast<int>(py)) + "px");
        if (nameEl_) nameEl_->SetInnerRML(name.c_str());
        panel_->SetClass("visible", true);
        panelVisible_ = true;
    }

    void hidePanel() {
        if (!panel_ || !panelVisible_) return;
        panel_->SetClass("visible", false);
        panelVisible_ = false;
    }

private:
    Rml::Element* panel_  = nullptr;
    Rml::Element* nameEl_ = nullptr;
    bool panelVisible_ = false;
};
