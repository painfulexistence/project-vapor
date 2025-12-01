#pragma once

#include <vector>
#include <unordered_map>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
// #include <concepts>

namespace Vapor {

/**
 * Concept for data definitions (static, immutable game data)
 *
 * DataDefinition represents the "template" or "blueprint" of game objects.
 * Examples: ItemDefinition, WeaponDefinition, QuestDefinition
 *
 * Requirements:
 *   - id: Unique identifier (uint32_t for performance)
 *   - name: Human-readable name
 *
 * Usage:
 *   // Game layer defines concrete types
 *   struct ItemDefinition {
 *       uint32_t id;
 *       std::string name;
 *       ItemType type;
 *       float weight;
 *       // ...
 *   };
 *   static_assert(DataDefinition<ItemDefinition>);
 *
 *   // Engine layer can write generic utilities
 *   template<DataDefinition Def>
 *   void debugPrint(const Def& def) {
 *       std::cout << def.id << ": " << def.name << std::endl;
 *   }
 *
 * Note:
 *   This concept defines minimal requirements. Most game code will
 *   use concrete types directly (e.g., ItemDefinition) rather than
 *   generic DataDefinition, because each system needs type-specific
 *   fields (damage, stackSize, cooldown, etc.).
 */
 template<typename T>
 concept DataDefinition = requires(const T& def) {
     { def.id } -> std::convertible_to<uint32_t>;
     { def.name } -> std::convertible_to<std::string_view>;
 };

 /**
  * Concept for data instances (runtime, mutable game state)
  *
  * DataInstance represents a runtime instance created from a Definition.
  * Examples: ItemInstance, WeaponInstance, QuestInstance
  *
  * Requirements:
  *   - instanceId: Unique runtime identifier
  *   - getDefinition(): Returns pointer to the source definition
  *
  * Usage:
  *   struct ItemInstance {
  *       uint32_t instanceId;
  *       const ItemDefinition* definition;
  *       uint32_t stackCount;
  *       // ...
  *
  *       const ItemDefinition* getDefinition() const { return definition; }
  *   };
  *   static_assert(DataInstance<ItemInstance>);
  *
  * Relationship:
  *   Definition (1) ←───── (*) Instance
  *   "Iron Sword"           Player's sword #1, #2, #3...
  *
  * Note:
  *   Like DataDefinition, most Components will use concrete Instance
  *   types directly. The concept is primarily for:
  *   - Documentation (what an Instance should have)
  *   - Generic utilities (serialization, debug UI, object pools)
  */
 template<typename T>
 concept DataInstance = requires(const T& inst) {
     { inst.instanceId } -> std::convertible_to<uint32_t>;
     { inst.getDefinition() } -> std::convertible_to<const void*>;
 };

 /**
  * Concept for typed data instances with known definition type
  *
  * Use this when you need compile-time knowledge of the Definition type.
  *
  * Usage:
  *   template<typename Inst>
  *       requires DataInstanceOf<Inst, ItemDefinition>
  *   void processItem(const Inst& item) {
  *       const ItemDefinition* def = item.getDefinition();
  *       // Now we know def is ItemDefinition*, not void*
  *   }
  */
 template<typename T, typename Def>
 concept DataInstanceOf = DataInstance<T> && DataDefinition<Def> &&
     requires(const T& inst) {
         { inst.getDefinition() } -> std::convertible_to<const Def*>;
     };

/**
 * Generic database for storing and querying data definitions
 *
 * DefinitionDatabase is a lightweight container optimized for:
 * - Fast ID-based lookup (O(1) via hash map)
 * - Cache-friendly iteration (contiguous memory storage)
 * - Code-driven registration (register definitions in code)
 *
 * Template parameter Def must satisfy the DataDefinition concept.
 *
 * Usage:
 *   // Game layer defines concrete definition type
 *   struct ItemDefinition {
 *       uint32_t id;
 *       std::string name;
 *       ItemType type;
 *       uint32_t maxStack;
 *   };
 *
 *   // Create database and register definitions
 *   DefinitionDatabase<ItemDefinition> itemDb;
 *   itemDb.registerDef({ 1001, "Health Potion", ItemType::Consumable, 99 });
 *   itemDb.registerDef({ 1002, "Mana Potion", ItemType::Consumable, 99 });
 *
 *   // Query
 *   const ItemDefinition* potion = itemDb.get(1001);
 *   for (const auto& item : itemDb.getAll()) { ... }
 */
template<DataDefinition Def>
class DefinitionDatabase {
public:
    DefinitionDatabase() = default;
    ~DefinitionDatabase() = default;

    // Non-copyable, movable
    DefinitionDatabase(const DefinitionDatabase&) = delete;
    DefinitionDatabase& operator=(const DefinitionDatabase&) = delete;
    DefinitionDatabase(DefinitionDatabase&&) = default;
    DefinitionDatabase& operator=(DefinitionDatabase&&) = default;

    // ========================================================================
    // Loading (Not implemented - for future data-driven extension)
    // ========================================================================

    /**
     * Load definitions from file (auto-detect format by extension)
     * @param path Path to .json or .bin file
     */
    void loadFromFile([[maybe_unused]] const std::string& path) {
        // TODO: Implement when data-driven loading is needed
    }

    /**
     * Load definitions from JSON file
     * @param path Path to .json file
     */
    void loadFromJson([[maybe_unused]] const std::string& path) {
        // TODO: Implement with Cereal JSON archive
    }

    /**
     * Load definitions from binary file
     * @param path Path to .bin file
     */
    void loadFromBinary([[maybe_unused]] const std::string& path) {
        // TODO: Implement with Cereal binary archive
    }

    // ========================================================================
    // Registration
    // ========================================================================

    /**
     * Register a single definition
     * @param def Definition to register
     * @throws std::runtime_error if ID already exists
     */
    void registerDef(const Def& def) {
        if (m_idIndex.contains(def.id)) {
            throw std::runtime_error("Definition ID already exists: " + std::to_string(def.id));
        }
        m_idIndex[def.id] = m_definitions.size();
        m_definitions.push_back(def);
    }

    /**
     * Register a single definition (move version)
     */
    void registerDef(Def&& def) {
        if (m_idIndex.contains(def.id)) {
            throw std::runtime_error("Definition ID already exists: " + std::to_string(def.id));
        }
        uint32_t id = def.id;
        m_idIndex[id] = m_definitions.size();
        m_definitions.push_back(std::move(def));
    }

    /**
     * Register multiple definitions at once
     * @param defs Span of definitions to register
     */
    void registerAll(std::span<const Def> defs) {
        m_definitions.reserve(m_definitions.size() + defs.size());
        for (const auto& def : defs) {
            registerDef(def);
        }
    }

    /**
     * Register or update a definition (upsert)
     * @param def Definition to register or update
     */
    void registerOrUpdate(const Def& def) {
        auto it = m_idIndex.find(def.id);
        if (it != m_idIndex.end()) {
            m_definitions[it->second] = def;
        } else {
            m_idIndex[def.id] = m_definitions.size();
            m_definitions.push_back(def);
        }
    }

    // ========================================================================
    // Query
    // ========================================================================

    /**
     * Get definition by ID
     * @param id Definition ID
     * @return Pointer to definition, or nullptr if not found
     */
    const Def* get(uint32_t id) const {
        auto it = m_idIndex.find(id);
        if (it != m_idIndex.end()) {
            return &m_definitions[it->second];
        }
        return nullptr;
    }

    /**
     * Check if definition exists
     * @param id Definition ID
     * @return true if exists
     */
    bool exists(uint32_t id) const {
        return m_idIndex.contains(id);
    }

    /**
     * Get all definitions (zero-copy view)
     * @return Span of all definitions
     */
    std::span<const Def> getAll() const {
        return m_definitions;
    }

    /**
     * Find definition by name (linear search)
     * @param name Definition name
     * @return Pointer to definition, or nullptr if not found
     */
    const Def* findByName(std::string_view name) const {
        for (const auto& def : m_definitions) {
            if (def.name == name) {
                return &def;
            }
        }
        return nullptr;
    }

    /**
     * Get number of registered definitions
     */
    size_t size() const {
        return m_definitions.size();
    }

    /**
     * Check if database is empty
     */
    bool empty() const {
        return m_definitions.empty();
    }

    // ========================================================================
    // Modification
    // ========================================================================

    /**
     * Clear all definitions
     */
    void clear() {
        m_definitions.clear();
        m_idIndex.clear();
    }

private:
    std::vector<Def> m_definitions;                    // Contiguous storage
    std::unordered_map<uint32_t, size_t> m_idIndex;   // ID -> index mapping
};

} // namespace Vapor