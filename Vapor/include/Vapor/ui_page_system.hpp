#pragma once
#include "rmlui_manager.hpp"
#include "ui_page.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Vapor {

    enum class PageID {
        HUD,
        Letterbox,
        Subtitle,
        ScrollText,
        ChapterTitle,
        MainMenu,
        PauseMenu,
        Settings,
        LoadingScreen,
    };

    // Tag: always-on overlay, not part of the menu stack
    struct UIOverlayTag {};

    // Tag: this page entity should currently be displayed
    struct UIVisibleTag {};

    // RmlUI document path + loaded state for one page entity
    struct UIDocumentComponent {
        std::string path;
        Rml::ElementDocument* doc = nullptr;
        bool lazyLoad = false;
        bool lastSentVisible = false;
    };

    // Owns the Page subclass (button binding, fade state machine)
    struct UIPageBehaviorComponent {
        std::shared_ptr<Page> page;
    };

    // Singleton: maps PageIDs to page entities and owns the menu navigation stack
    struct UINavigatorComponent {
        std::unordered_map<PageID, entt::entity> pages;
        std::vector<entt::entity> stack;
    };

    class PageSystem {
    public:
        static void update(entt::registry& reg, RmlUiManager* rml, float dt) {
            if (!rml) return;
            auto view = reg.view<UIDocumentComponent, UIPageBehaviorComponent>();
            for (auto entity : view) {
                auto& doc = view.get<UIDocumentComponent>(entity);
                auto& behavior = view.get<UIPageBehaviorComponent>(entity);

                if (!doc.doc) {
                    bool shouldLoad = !doc.lazyLoad || reg.all_of<UIVisibleTag>(entity);
                    if (shouldLoad) {
                        auto* d = rml->LoadDocument(doc.path);
                        if (!d) {
                            fmt::print(stderr, "PageSystem: failed to load '{}'\n", doc.path);
                            continue;
                        }
                        doc.doc = d;
                        behavior.page->onAttach(d, reg);
                    }
                }

                if (!doc.doc) continue;

                bool visible = reg.all_of<UIVisibleTag>(entity);
                if (visible != doc.lastSentVisible) {
                    if (visible)
                        behavior.page->show();
                    else
                        behavior.page->hide();
                    doc.lastSentVisible = visible;
                }

                behavior.page->onUpdate(dt);
            }
        }

        static void show(entt::registry& reg, PageID id) {
            auto e = findEntity(reg, id);
            if (e != entt::null) reg.emplace_or_replace<UIVisibleTag>(e);
        }

        static void hide(entt::registry& reg, PageID id) {
            auto e = findEntity(reg, id);
            if (e != entt::null) reg.remove<UIVisibleTag>(e);
        }

        static void push(entt::registry& reg, PageID id) {
            auto& nav = getNavigator(reg);
            if (!nav.stack.empty()) reg.remove<UIVisibleTag>(nav.stack.back());
            auto it = nav.pages.find(id);
            if (it == nav.pages.end()) return;
            nav.stack.push_back(it->second);
            reg.emplace_or_replace<UIVisibleTag>(it->second);
        }

        static void pop(entt::registry& reg) {
            auto& nav = getNavigator(reg);
            if (nav.stack.empty()) return;
            reg.remove<UIVisibleTag>(nav.stack.back());
            nav.stack.pop_back();
            if (!nav.stack.empty()) reg.emplace_or_replace<UIVisibleTag>(nav.stack.back());
        }

        static void popAll(entt::registry& reg) {
            auto& nav = getNavigator(reg);
            for (auto e : nav.stack)
                reg.remove<UIVisibleTag>(e);
            nav.stack.clear();
        }

        static bool isTopOfStack(entt::registry& reg, PageID id) {
            auto& nav = getNavigator(reg);
            if (nav.stack.empty()) return false;
            auto it = nav.pages.find(id);
            return it != nav.pages.end() && nav.stack.back() == it->second;
        }

        static bool isStackEmpty(entt::registry& reg) {
            return getNavigator(reg).stack.empty();
        }

        template<typename T>
        static T* getPage(entt::registry& reg, PageID id) {
            auto e = findEntity(reg, id);
            if (e == entt::null) return nullptr;
            auto* b = reg.try_get<UIPageBehaviorComponent>(e);
            return b ? static_cast<T*>(b->page.get()) : nullptr;
        }

    private:
        static UINavigatorComponent& getNavigator(entt::registry& reg) {
            auto view = reg.view<UINavigatorComponent>();
            return view.get<UINavigatorComponent>(view.front());
        }

        static entt::entity findEntity(entt::registry& reg, PageID id) {
            auto& nav = getNavigator(reg);
            auto it = nav.pages.find(id);
            return it != nav.pages.end() ? it->second : entt::null;
        }
    };

}// namespace Vapor
