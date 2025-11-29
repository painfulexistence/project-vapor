#ifndef DATA_DEFINITION_HPP
#define DATA_DEFINITION_HPP

#include <concepts>
#include <string>
#include <string_view>
#include <cstdint>

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

} // namespace Vapor

#endif // DATA_DEFINITION_HPP
