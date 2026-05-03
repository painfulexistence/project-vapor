#pragma once
#include "page.hpp"
#include "Vapor/rmlui_manager.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

struct UIStateComponent {
    struct Entry {
        std::string documentPath;
        std::unique_ptr<Page> page;
        bool shouldBeVisible = false;
        bool lazyLoad = false;   // if true, document is loaded only when first shown
        bool lastSentVisible = false;
    };
    std::unordered_map<PageID, Entry> pages;
    std::vector<PageID> menuStack;
};

class PageSystem {
public:
    static void update(entt::registry& reg, Vapor::RmlUiManager* rml, float dt) {
        if (!rml) return;
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view) {
            auto& ui = view.get<UIStateComponent>(entity);
            for (auto& [id, entry] : ui.pages) {
                if (!entry.page) continue;

                if (!entry.page->doc_) {
                    bool shouldLoad = !entry.lazyLoad || entry.shouldBeVisible;
                    if (shouldLoad) {
                        auto* doc = rml->LoadDocument(entry.documentPath);
                        if (!doc) {
                            fmt::print(stderr, "PageSystem: failed to load '{}'\n", entry.documentPath);
                            continue;
                        }
                        entry.page->onAttach(doc, reg);
                    }
                }

                if (!entry.page->doc_) continue;

                if (entry.shouldBeVisible != entry.lastSentVisible) {
                    if (entry.shouldBeVisible) entry.page->show();
                    else entry.page->hide();
                    entry.lastSentVisible = entry.shouldBeVisible;
                }

                entry.page->onUpdate(dt);
            }
        }
    }

    static void show(entt::registry& reg, PageID id) { setVisible(reg, id, true); }
    static void hide(entt::registry& reg, PageID id) { setVisible(reg, id, false); }

    static void push(entt::registry& reg, PageID id) {
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view) {
            auto& ui = view.get<UIStateComponent>(entity);
            if (!ui.menuStack.empty())
                setPageVisible(ui, ui.menuStack.back(), false);
            ui.menuStack.push_back(id);
            setPageVisible(ui, id, true);
        }
    }

    static void pop(entt::registry& reg) {
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view) {
            auto& ui = view.get<UIStateComponent>(entity);
            if (ui.menuStack.empty()) return;
            setPageVisible(ui, ui.menuStack.back(), false);
            ui.menuStack.pop_back();
            if (!ui.menuStack.empty())
                setPageVisible(ui, ui.menuStack.back(), true);
        }
    }

    static void popAll(entt::registry& reg) {
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view) {
            auto& ui = view.get<UIStateComponent>(entity);
            for (auto id : ui.menuStack)
                setPageVisible(ui, id, false);
            ui.menuStack.clear();
        }
    }

    template<typename T>
    static T* getPage(entt::registry& reg, PageID id) {
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view) {
            auto& ui = view.get<UIStateComponent>(entity);
            auto it = ui.pages.find(id);
            if (it != ui.pages.end() && it->second.page)
                return static_cast<T*>(it->second.page.get());
        }
        return nullptr;
    }

private:
    static void setVisible(entt::registry& reg, PageID id, bool visible) {
        auto view = reg.view<UIStateComponent>();
        for (auto entity : view)
            setPageVisible(view.get<UIStateComponent>(entity), id, visible);
    }

    static void setPageVisible(UIStateComponent& ui, PageID id, bool visible) {
        auto it = ui.pages.find(id);
        if (it != ui.pages.end())
            it->second.shouldBeVisible = visible;
    }
};
