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

// Helper: create a page entity pre-loaded (bypasses document loading in update)
static entt::entity makePage(entt::registry& reg, UINavigatorComponent& nav,
                              PageID id, std::shared_ptr<MockPage> page,
                              const std::string& path = "dummy/path") {
    auto e = reg.create();
    reg.emplace<UIDocumentComponent>(e, UIDocumentComponent{
        .path = path,
        .doc  = reinterpret_cast<Rml::ElementDocument*>(0xdeadbeef),
    });
    reg.emplace<UIPageBehaviorComponent>(e, UIPageBehaviorComponent{ .page = page });
    nav.pages[id] = e;
    return e;
}

// --- Tests ---

TEST_CASE("UI System - Basic Visibility", "[ui]") {
    entt::registry reg;
    auto navEntity = reg.create();
    auto& nav = reg.emplace<UINavigatorComponent>(navEntity);

    auto page = std::make_shared<MockPage>();
    auto hudEntity = makePage(reg, nav, PageID::HUD, page);

    SECTION("Initial state is hidden") {
        CHECK(page->isVisible == false);
        CHECK(!reg.all_of<UIVisibleTag>(hudEntity));
    }

    SECTION("Showing page sets UIVisibleTag and calls page->show()") {
        PageSystem::show(reg, PageID::HUD);
        CHECK(reg.all_of<UIVisibleTag>(hudEntity));

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(page->isVisible == true);
    }

    SECTION("Hiding page removes UIVisibleTag and calls page->hide()") {
        PageSystem::show(reg, PageID::HUD);
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);

        PageSystem::hide(reg, PageID::HUD);
        CHECK(!reg.all_of<UIVisibleTag>(hudEntity));

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(page->isVisible == false);
    }
}

TEST_CASE("UI System - Menu Stack (Push/Pop)", "[ui]") {
    entt::registry reg;
    auto navEntity = reg.create();
    auto& nav = reg.emplace<UINavigatorComponent>(navEntity);

    auto menuA = std::make_shared<MockPage>();
    auto menuB = std::make_shared<MockPage>();
    auto mainMenuEntity = makePage(reg, nav, PageID::MainMenu, menuA, "path/a");
    auto pauseMenuEntity = makePage(reg, nav, PageID::PauseMenu, menuB, "path/b");

    SECTION("Pushing a menu shows it") {
        PageSystem::push(reg, PageID::MainMenu);
        CHECK(nav.stack.size() == 1);
        CHECK(reg.all_of<UIVisibleTag>(mainMenuEntity));

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == true);
    }

    SECTION("Pushing a second menu hides the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);

        CHECK(nav.stack.size() == 2);
        CHECK(!reg.all_of<UIVisibleTag>(mainMenuEntity));
        CHECK(reg.all_of<UIVisibleTag>(pauseMenuEntity));

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == false);
        CHECK(menuB->isVisible == true);
    }

    SECTION("Popping the second menu restores the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);
        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);

        PageSystem::pop(reg);

        CHECK(nav.stack.size() == 1);
        CHECK(nav.stack.back() == mainMenuEntity);
        CHECK(reg.all_of<UIVisibleTag>(mainMenuEntity));
        CHECK(!reg.all_of<UIVisibleTag>(pauseMenuEntity));

        PageSystem::update(reg, reinterpret_cast<RmlUiManager*>(0x1), 0.016f);
        CHECK(menuA->isVisible == true);
        CHECK(menuB->isVisible == false);
    }

    SECTION("isTopOfStack and isStackEmpty helpers") {
        CHECK(PageSystem::isStackEmpty(reg));
        CHECK(!PageSystem::isTopOfStack(reg, PageID::MainMenu));

        PageSystem::push(reg, PageID::MainMenu);
        CHECK(!PageSystem::isStackEmpty(reg));
        CHECK(PageSystem::isTopOfStack(reg, PageID::MainMenu));
        CHECK(!PageSystem::isTopOfStack(reg, PageID::PauseMenu));

        PageSystem::push(reg, PageID::PauseMenu);
        CHECK(PageSystem::isTopOfStack(reg, PageID::PauseMenu));
        CHECK(!PageSystem::isTopOfStack(reg, PageID::MainMenu));
    }
}
