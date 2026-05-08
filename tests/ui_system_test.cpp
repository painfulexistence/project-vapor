#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>
#include "Vapor/ui_page_system.hpp"

using namespace Vapor;

// --- Mocking ---

class MockPage : public Page {
public:
    int updateCount = 0;
    bool isVisible = false;

    void onUpdate(float dt) override { updateCount++; }
    void show() override { isVisible = true; Vapor::Page::show(); }
    void hide() override { isVisible = false; Vapor::Page::hide(); }
};

// --- Tests ---

TEST_CASE("UI System - Basic Visibility", "[ui]") {
    entt::registry reg;
    auto entity = reg.create();
    auto& ui = reg.emplace<UIStateComponent>(entity);

    auto page = std::make_shared<MockPage>();
    page->onAttach(reinterpret_cast<Rml::ElementDocument*>(0xdeadbeef), reg);
    
    ui.pages[PageID::HUD] = { "dummy/path", page };

    SECTION("Initial state is hidden") {
        CHECK(page->isVisible == false);
        CHECK(ui.pages[PageID::HUD].shouldBeVisible == false);
    }

    SECTION("Showing page updates state and calls page->show()") {
        PageSystem::show(reg, PageID::HUD);
        CHECK(ui.pages[PageID::HUD].shouldBeVisible == true);
        
        // Use a non-null pointer for RmlUiManager to pass the guard
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(page->isVisible == true);
    }

    SECTION("Hiding page updates state and calls page->hide()") {
        PageSystem::show(reg, PageID::HUD);
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        
        PageSystem::hide(reg, PageID::HUD);
        CHECK(ui.pages[PageID::HUD].shouldBeVisible == false);
        
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(page->isVisible == false);
    }
}

TEST_CASE("UI System - Menu Stack (Push/Pop)", "[ui]") {
    entt::registry reg;
    auto entity = reg.create();
    auto& ui = reg.emplace<UIStateComponent>(entity);

    auto menuA = std::make_shared<MockPage>();
    auto menuB = std::make_shared<MockPage>();
    menuA->onAttach(reinterpret_cast<Rml::ElementDocument*>(0x1), reg);
    menuB->onAttach(reinterpret_cast<Rml::ElementDocument*>(0x2), reg);

    ui.pages[PageID::MainMenu] = { "path/a", menuA };
    ui.pages[PageID::PauseMenu] = { "path/b", menuB };

    SECTION("Pushing a menu shows it") {
        PageSystem::push(reg, PageID::MainMenu);
        CHECK(ui.menuStack.size() == 1);
        CHECK(ui.pages[PageID::MainMenu].shouldBeVisible == true);
        
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == true);
    }

    SECTION("Pushing a second menu hides the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);
        
        CHECK(ui.menuStack.size() == 2);
        CHECK(ui.pages[PageID::MainMenu].shouldBeVisible == false);
        CHECK(ui.pages[PageID::PauseMenu].shouldBeVisible == true);

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == false);
        CHECK(menuB->isVisible == true);
    }

    SECTION("Popping the second menu restores the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        
        PageSystem::pop(reg);
        
        CHECK(ui.menuStack.size() == 1);
        CHECK(ui.menuStack.back() == PageID::MainMenu);
        CHECK(ui.pages[PageID::MainMenu].shouldBeVisible == true);
        CHECK(ui.pages[PageID::PauseMenu].shouldBeVisible == false);

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == true);
        CHECK(menuB->isVisible == false);
    }
}
