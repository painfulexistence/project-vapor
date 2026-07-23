#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include "Vapor/ui_document_registry.hpp"

using namespace Vapor;

// Pure bookkeeping under test — the registry never dereferences the document
// pointer, so opaque fake pointers are the honest fixture here (unlike the old
// ui_system_test, which had to smuggle 0xdeadbeef into a raw-pointer field).

static Rml::ElementDocument* fake(uintptr_t id) {
    return reinterpret_cast<Rml::ElementDocument*>(id);
}

TEST_CASE("UIDocumentRegistry - insert/resolve/path", "[ui][registry]") {
    UIDocumentRegistry reg;

    const UIDocumentHandle h = reg.insert(fake(0x10), "ui/hud.rml");
    REQUIRE(h.valid());
    CHECK(reg.resolve(h) == fake(0x10));
    REQUIRE(reg.path(h) != nullptr);
    CHECK(*reg.path(h) == "ui/hud.rml");
    CHECK(reg.version(h) == 1);
    CHECK(reg.liveCount() == 1);

    SECTION("default handle is invalid and resolves null") {
        UIDocumentHandle none;
        CHECK(!none.valid());
        CHECK(reg.resolve(none) == nullptr);
        CHECK(reg.path(none) == nullptr);
        CHECK(reg.version(none) == 0);
    }

    SECTION("distinct documents get distinct handles") {
        const UIDocumentHandle h2 = reg.insert(fake(0x20), "ui/menu.rml");
        CHECK(h2.valid());
        CHECK(!(h2 == h));
        CHECK(reg.resolve(h) == fake(0x10));
        CHECK(reg.resolve(h2) == fake(0x20));
        CHECK(reg.liveCount() == 2);
    }
}

TEST_CASE("UIDocumentRegistry - replace keeps the handle, bumps the version", "[ui][registry]") {
    UIDocumentRegistry reg;
    const UIDocumentHandle h = reg.insert(fake(0x10), "ui/hud.rml");

    reg.replace(h, fake(0x11));
    CHECK(reg.resolve(h) == fake(0x11));
    CHECK(reg.version(h) == 2);

    // Failed reload: slot resolves null but the handle stays live (a later
    // replace can revive it); version still moves so watchers detach.
    reg.replace(h, nullptr);
    CHECK(reg.resolve(h) == nullptr);
    CHECK(reg.version(h) == 3);
    CHECK(reg.liveCount() == 1);
}

TEST_CASE("UIDocumentRegistry - erase makes handles stale immediately", "[ui][registry]") {
    UIDocumentRegistry reg;
    const UIDocumentHandle h = reg.insert(fake(0x10), "ui/hud.rml");

    reg.erase(h);
    CHECK(reg.resolve(h) == nullptr);
    CHECK(reg.path(h) == nullptr);
    CHECK(reg.version(h) == 0);
    CHECK(reg.liveCount() == 0);

    SECTION("erase is idempotent on a stale handle") {
        reg.erase(h);
        CHECK(reg.liveCount() == 0);
    }

    SECTION("slot reuse does not resurrect the stale handle") {
        const UIDocumentHandle h2 = reg.insert(fake(0x20), "ui/menu.rml");
        // The freed slot is reused, but the generation moved on.
        CHECK(h2.slot == h.slot);
        CHECK(h2.gen != h.gen);
        CHECK(reg.resolve(h) == nullptr);// old handle still stale
        CHECK(reg.resolve(h2) == fake(0x20));
        CHECK(reg.version(h2) == 1);// fresh document, fresh version
    }
}
