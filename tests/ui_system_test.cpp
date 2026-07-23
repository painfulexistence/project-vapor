#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entt.hpp>
#include "Vapor/ui_page_system.hpp"

using namespace Vapor;

// --- Mocking ---
//
// A real RmlUiManager is constructed WITHOUT Initialize(): its document
// registry is plain bookkeeping (no Rml calls), so tests seed it with opaque
// fake pointers through the same insert/resolve path production uses. Nothing
// in PageSystem dereferences the document — MockPage ignores it — so the fake
// pointers only ever travel, never get used.

class MockPage : public Page {
public:
    int updateCount = 0;
    int attachCount = 0;
    int detachCount = 0;
    bool isVisible = false;

    void onAttach(Rml::ElementDocument* doc, entt::registry& reg) override {
        Page::onAttach(doc, reg);
        attachCount++;
    }
    void onDetach() override {
        Page::onDetach();
        detachCount++;
    }
    void onUpdate(float dt) override { updateCount++; }
    void show() override { isVisible = true; Vapor::Page::show(); }
    void hide() override { isVisible = false; Vapor::Page::hide(); }
};

static Rml::ElementDocument* fakeDoc(uintptr_t id) {
    return reinterpret_cast<Rml::ElementDocument*>(id);
}

// Helper: create a page entity whose document is already registered (bypasses
// document loading in update — the handle is live, so update() only attaches).
static entt::entity makePage(entt::registry& reg, RmlUiManager& rml, UINavigatorComponent& nav,
                              PageID id, std::shared_ptr<MockPage> page,
                              const std::string& path = "dummy/path",
                              uintptr_t fakePtr = 0x1000) {
    auto e = reg.create();
    reg.emplace<UIDocumentComponent>(e, UIDocumentComponent{
        .path = path,
        .doc  = rml.documents().insert(fakeDoc(fakePtr), path),
    });
    reg.emplace<UIPageBehaviorComponent>(e, UIPageBehaviorComponent{ .page = page });
    nav.pages[id] = e;
    return e;
}

// --- Tests ---

TEST_CASE("UI System - Basic Visibility", "[ui]") {
    entt::registry reg;
    RmlUiManager rml;
    auto navEntity = reg.create();
    auto& nav = reg.emplace<UINavigatorComponent>(navEntity);

    auto page = std::make_shared<MockPage>();
    auto hudEntity = makePage(reg, rml, nav, PageID::HUD, page);

    SECTION("Initial state is hidden") {
        CHECK(page->isVisible == false);
        CHECK(!reg.all_of<UIVisibleTag>(hudEntity));
    }

    SECTION("First update attaches the page against the live document") {
        CHECK(page->attachCount == 0);
        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->attachCount == 1);
        CHECK(page->updateCount == 1);

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->attachCount == 1);// same version — no re-attach
    }

    SECTION("Showing page sets UIVisibleTag and calls page->show()") {
        PageSystem::show(reg, PageID::HUD);
        CHECK(reg.all_of<UIVisibleTag>(hudEntity));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->isVisible == true);
    }

    SECTION("Hiding page removes UIVisibleTag and calls page->hide()") {
        PageSystem::show(reg, PageID::HUD);
        PageSystem::update(reg, &rml, 0.016f);

        PageSystem::hide(reg, PageID::HUD);
        CHECK(!reg.all_of<UIVisibleTag>(hudEntity));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->isVisible == false);
    }
}

TEST_CASE("UI System - Menu Stack (Push/Pop)", "[ui]") {
    entt::registry reg;
    RmlUiManager rml;
    auto navEntity = reg.create();
    auto& nav = reg.emplace<UINavigatorComponent>(navEntity);

    auto menuA = std::make_shared<MockPage>();
    auto menuB = std::make_shared<MockPage>();
    auto mainMenuEntity = makePage(reg, rml, nav, PageID::MainMenu, menuA, "path/a", 0x1000);
    auto pauseMenuEntity = makePage(reg, rml, nav, PageID::PauseMenu, menuB, "path/b", 0x2000);

    SECTION("Pushing a menu shows it") {
        PageSystem::push(reg, PageID::MainMenu);
        CHECK(nav.stack.size() == 1);
        CHECK(reg.all_of<UIVisibleTag>(mainMenuEntity));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(menuA->isVisible == true);
    }

    SECTION("Pushing a second menu hides the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);

        CHECK(nav.stack.size() == 2);
        CHECK(!reg.all_of<UIVisibleTag>(mainMenuEntity));
        CHECK(reg.all_of<UIVisibleTag>(pauseMenuEntity));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(menuA->isVisible == false);
        CHECK(menuB->isVisible == true);
    }

    SECTION("Popping the second menu restores the first one") {
        PageSystem::push(reg, PageID::MainMenu);
        PageSystem::push(reg, PageID::PauseMenu);
        PageSystem::update(reg, &rml, 0.016f);

        PageSystem::pop(reg);

        CHECK(nav.stack.size() == 1);
        CHECK(nav.stack.back() == mainMenuEntity);
        CHECK(reg.all_of<UIVisibleTag>(mainMenuEntity));
        CHECK(!reg.all_of<UIVisibleTag>(pauseMenuEntity));

        PageSystem::update(reg, &rml, 0.016f);
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

TEST_CASE("UI System - Document Handle Lifetime", "[ui]") {
    entt::registry reg;
    RmlUiManager rml;
    auto navEntity = reg.create();
    auto& nav = reg.emplace<UINavigatorComponent>(navEntity);

    auto page = std::make_shared<MockPage>();
    auto hudEntity = makePage(reg, rml, nav, PageID::HUD, page);
    auto& doc = reg.get<UIDocumentComponent>(hudEntity);

    PageSystem::update(reg, &rml, 0.016f);
    REQUIRE(page->attachCount == 1);

    SECTION("Hot reload (replace) re-attaches against the new document") {
        rml.documents().replace(doc.doc, fakeDoc(0x9000));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->detachCount == 1);
        CHECK(page->attachCount == 2);
        CHECK(page->document() == fakeDoc(0x9000));

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->attachCount == 2);// version stable again
    }

    SECTION("Closing the document detaches the page and clears the handle") {
        const UIDocumentHandle old = doc.doc;
        rml.documents().erase(old);

        CHECK(rml.documents().resolve(old) == nullptr);// stale immediately

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->detachCount == 1);
        CHECK(!doc.doc.valid());
        CHECK(doc.seenVersion == 0);
    }

    SECTION("A failed reload (null replace) detaches and allows retry") {
        rml.documents().replace(doc.doc, nullptr);

        PageSystem::update(reg, &rml, 0.016f);
        CHECK(page->detachCount == 1);
        CHECK(!doc.doc.valid());// dropped; lazy rules would reload by path
    }
}
