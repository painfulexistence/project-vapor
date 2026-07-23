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

    // RmlUI document reference for one page entity. Pure data: the document
    // itself is owned by the Rml context and named by handle — never by raw
    // pointer — so the component stays value-semantic (blueprint-authorable,
    // PFR-inspectable, copyable without aliasing a lifetime it doesn't own).
    struct UIDocumentComponent {
        std::string path;
        UIDocumentHandle doc;       // runtime; resolved through RmlUiManager each frame
        Uint32 seenVersion = 0;     // document version the page last attached against (0 = never)
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

                if (!doc.doc.valid()) {
                    bool shouldLoad = !doc.lazyLoad || reg.all_of<UIVisibleTag>(entity);
                    if (shouldLoad) {
                        doc.doc = rml->LoadDocumentHandle(doc.path);
                        if (!doc.doc.valid()) {
                            fmt::print(stderr, "PageSystem: failed to load '{}'\n", doc.path);
                            continue;
                        }
                    }
                }

                if (!doc.doc.valid()) continue;

                // Re-resolve every frame: the handle, not the pointer, is the
                // stable identity. Null means the document was closed behind us
                // (or a hot reload failed) — drop the handle so the lazy rules
                // above decide whether to load again next frame.
                Rml::ElementDocument* d = rml->Resolve(doc.doc);
                if (!d) {
                    if (doc.seenVersion != 0) behavior.page->onDetach();
                    doc.doc = {};
                    doc.seenVersion = 0;
                    continue;
                }

                // First attach and every hot reload land here: the registry
                // bumps the version when the pointer changes, and the page
                // re-resolves its element caches / listeners in onAttach.
                const Uint32 ver = rml->DocumentVersion(doc.doc);
                if (ver != doc.seenVersion) {
                    if (doc.seenVersion != 0) behavior.page->onDetach();
                    behavior.page->onAttach(d, reg);
                    doc.seenVersion = ver;
                }

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

        // Hot-reload one page's document in place. The page is detached FIRST
        // (its bind() listeners are removed while the old elements are alive),
        // then the manager swaps the document under the same handle; the next
        // update() sees the version bump and re-attaches against the new DOM.
        static void reload(entt::registry& reg, RmlUiManager* rml, PageID id) {
            if (!rml) return;
            auto e = findEntity(reg, id);
            if (e == entt::null) return;
            auto* doc = reg.try_get<UIDocumentComponent>(e);
            auto* behavior = reg.try_get<UIPageBehaviorComponent>(e);
            if (!doc || !doc->doc.valid()) return;
            if (behavior && behavior->page && doc->seenVersion != 0) {
                behavior->page->onDetach();
                doc->seenVersion = 0;
            }
            rml->ReloadDocument(doc->doc);
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
            auto* nav = getNavigator(reg);
            if (!nav) return;
            if (!nav->stack.empty()) reg.remove<UIVisibleTag>(nav->stack.back());
            auto it = nav->pages.find(id);
            if (it == nav->pages.end()) return;
            nav->stack.push_back(it->second);
            reg.emplace_or_replace<UIVisibleTag>(it->second);
        }

        static void pop(entt::registry& reg) {
            auto* nav = getNavigator(reg);
            if (!nav || nav->stack.empty()) return;
            reg.remove<UIVisibleTag>(nav->stack.back());
            nav->stack.pop_back();
            if (!nav->stack.empty()) reg.emplace_or_replace<UIVisibleTag>(nav->stack.back());
        }

        static void popAll(entt::registry& reg) {
            auto* nav = getNavigator(reg);
            if (!nav) return;
            for (auto e : nav->stack)
                reg.remove<UIVisibleTag>(e);
            nav->stack.clear();
        }

        static bool isTopOfStack(entt::registry& reg, PageID id) {
            auto* nav = getNavigator(reg);
            if (!nav || nav->stack.empty()) return false;
            auto it = nav->pages.find(id);
            return it != nav->pages.end() && nav->stack.back() == it->second;
        }

        static bool isStackEmpty(entt::registry& reg) {
            auto* nav = getNavigator(reg);
            return !nav || nav->stack.empty();
        }

        template<typename T>
        static T* getPage(entt::registry& reg, PageID id) {
            auto e = findEntity(reg, id);
            if (e == entt::null) return nullptr;
            auto* b = reg.try_get<UIPageBehaviorComponent>(e);
            return b ? static_cast<T*>(b->page.get()) : nullptr;
        }

    private:
        // Returns nullptr when no UINavigatorComponent exists yet. Every caller
        // must guard: a missing navigator is a normal early-startup state (and,
        // in a build without Boost.PFR reflection, the blueprint applier that
        // would emplace it is compiled out entirely). The old version returned a
        // reference via view.get(view.front()); with an empty view front() is
        // entt::null, so that dereferenced an invalid entity and crashed.
        static UINavigatorComponent* getNavigator(entt::registry& reg) {
            auto view = reg.view<UINavigatorComponent>();
            const entt::entity e = view.front();
            if (e == entt::null) return nullptr;
            return &view.get<UINavigatorComponent>(e);
        }

        static entt::entity findEntity(entt::registry& reg, PageID id) {
            auto* nav = getNavigator(reg);
            if (!nav) return entt::null;
            auto it = nav->pages.find(id);
            return it != nav->pages.end() ? it->second : entt::null;
        }
    };

}// namespace Vapor
