#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Rml {
    class ElementDocument;
}

namespace Vapor {

    // Handle to a UI document slot in UIDocumentRegistry. Follows the house
    // handle style (PhysicsHandle / GPUHandle: 32-bit id + UINT32_MAX sentinel),
    // plus a generation: unlike bodies or atlases, documents are closed and
    // reloaded as a designed workflow (hot reload), so a stale handle must be
    // detectable rather than silently resolving to a recycled slot.
    struct UIDocumentHandle {
        uint32_t slot = UINT32_MAX;
        uint32_t gen = 0;

        bool valid() const {
            return slot != UINT32_MAX;
        }
        bool operator==(const UIDocumentHandle&) const = default;
    };

    // Bookkeeping between UIDocumentHandles and the Rml documents they name.
    // Owned by RmlUiManager; deliberately makes NO RmlUi calls (the pointer is
    // opaque here), so it is unit-testable without Rml::Initialise().
    //
    // Lifetime protocol (RmlUiManager enforces it):
    //   - insert() when a document is loaded; the handle stays stable for the
    //     document's whole life, across hot reloads.
    //   - replace() on hot reload: same slot, new pointer, version bumped.
    //     Callers that cached element pointers detect the bump and re-resolve.
    //   - erase() when the document is closed: the slot's generation bumps, so
    //     every outstanding handle goes stale IMMEDIATELY — Rml defers actual
    //     document destruction to the next Context::Update, and nothing must
    //     resolve the document inside that window.
    class UIDocumentRegistry {
    public:
        UIDocumentHandle insert(Rml::ElementDocument* doc, std::string path) {
            uint32_t slot;
            if (!m_free.empty()) {
                slot = m_free.back();
                m_free.pop_back();
            } else {
                slot = static_cast<uint32_t>(m_slots.size());
                m_slots.push_back({});
            }
            Slot& s = m_slots[slot];
            s.doc = doc;
            s.path = std::move(path);
            s.version = 1;
            s.alive = true;
            return { slot, s.gen };
        }

        Rml::ElementDocument* resolve(UIDocumentHandle h) const {
            const Slot* s = get(h);
            return s ? s->doc : nullptr;
        }

        // The path the document was loaded from (as authored, not resolved) —
        // what reload feeds back into LoadDocument. Null for stale handles.
        const std::string* path(UIDocumentHandle h) const {
            const Slot* s = get(h);
            return s ? &s->path : nullptr;
        }

        // Bumped by replace(); 0 for stale/invalid handles. Callers cache the
        // version they attached against and re-attach when it moves.
        uint32_t version(UIDocumentHandle h) const {
            const Slot* s = get(h);
            return s ? s->version : 0;
        }

        // Hot reload: the handle keeps working, the pointer changes underneath.
        // newDoc may be null (reload failed) — the slot then resolves null until
        // a later replace(), letting callers drop and retry by path.
        void replace(UIDocumentHandle h, Rml::ElementDocument* newDoc) {
            if (Slot* s = getMutable(h)) {
                s->doc = newDoc;
                s->version++;
            }
        }

        void erase(UIDocumentHandle h) {
            if (Slot* s = getMutable(h)) {
                s->doc = nullptr;
                s->path.clear();
                s->gen++;// every outstanding handle to this slot is now stale
                s->alive = false;
                m_free.push_back(h.slot);
            }
        }

        size_t liveCount() const {
            size_t n = 0;
            for (const Slot& s : m_slots)
                if (s.alive) n++;
            return n;
        }

    private:
        struct Slot {
            Rml::ElementDocument* doc = nullptr;
            std::string path;
            uint32_t gen = 0;
            uint32_t version = 0;
            bool alive = false;
        };

        const Slot* get(UIDocumentHandle h) const {
            if (!h.valid() || h.slot >= m_slots.size()) return nullptr;
            const Slot& s = m_slots[h.slot];
            return (s.alive && s.gen == h.gen) ? &s : nullptr;
        }
        Slot* getMutable(UIDocumentHandle h) {
            return const_cast<Slot*>(get(h));
        }

        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_free;
    };

}// namespace Vapor
