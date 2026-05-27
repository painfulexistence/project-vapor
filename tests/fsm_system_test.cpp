// Unit tests for the FSM System (FSMDefinitionBuilder, updateFSMSystem, state transitions).
#include <Vapor/fsm.hpp>
#include <Vapor/fsm_system.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>

using namespace Vapor;
using Catch::Approx;

// ============================================================
// FSMDefinitionBuilder
// ============================================================

TEST_CASE("FSMDefinitionBuilder 基本建構", "[fsm][builder]") {
    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .state("Running")
        .initialState("Idle")
        .build();

    REQUIRE(def.stateNames.size() == 3);
    REQUIRE(def.stateNames[0] == "Idle");
    REQUIRE(def.stateNames[1] == "Walking");
    REQUIRE(def.stateNames[2] == "Running");
    REQUIRE(def.initialState == 0);
}

TEST_CASE("FSMDefinitionBuilder 事件轉換", "[fsm][builder]") {
    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .transition("Idle", "Walking", "StartWalk")
        .transition("Walking", "Idle", "Stop")
        .build();

    REQUIRE(def.eventTransitions.size() == 2);
    REQUIRE(def.eventTransitions[0].fromState == 0);
    REQUIRE(def.eventTransitions[0].toState == 1);
    REQUIRE(def.eventTransitions[0].triggerEvent == "StartWalk");
    REQUIRE(def.eventTransitions[1].fromState == 1);
    REQUIRE(def.eventTransitions[1].toState == 0);
    REQUIRE(def.eventTransitions[1].triggerEvent == "Stop");
}

TEST_CASE("FSMDefinitionBuilder 時間轉換", "[fsm][builder]") {
    auto def = FSMDefinitionBuilder()
        .state("Attack")
        .state("Idle")
        .timedTransition("Attack", "Idle", 0.5f)
        .build();

    REQUIRE(def.timedTransitions.size() == 1);
    REQUIRE(def.timedTransitions[0].fromState == 0);
    REQUIRE(def.timedTransitions[0].toState == 1);
    REQUIRE(def.timedTransitions[0].duration == Approx(0.5f));
}

TEST_CASE("FSMDefinitionBuilder 自動建立未宣告狀態", "[fsm][builder]") {
    auto def = FSMDefinitionBuilder()
        .transition("A", "B", "GoToB")
        .transition("B", "C", "GoToC")
        .build();

    REQUIRE(def.stateNames.size() == 3);
    REQUIRE(def.getStateIndex("A") == 0);
    REQUIRE(def.getStateIndex("B") == 1);
    REQUIRE(def.getStateIndex("C") == 2);
}

TEST_CASE("FSMDefinition::getStateName 邊界檢查", "[fsm][definition]") {
    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walk")
        .build();

    REQUIRE(def.getStateName(0) == "Idle");
    REQUIRE(def.getStateName(1) == "Walk");
    REQUIRE(def.getStateName(999) == "");
}

// ============================================================
// initFSM
// ============================================================

TEST_CASE("initFSM 初始化狀態", "[fsm][init]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Active")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);

    initFSM(reg, entity, def);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.currentState == 0);
    REQUIRE(state.stateTime == Approx(0.0f));
    REQUIRE(state.totalTime == Approx(0.0f));

    // Should emit initial state change event
    REQUIRE(reg.all_of<FSMStateChangeEvent>(entity));
    auto& event = reg.get<FSMStateChangeEvent>(entity);
    REQUIRE(event.fromState == 0);
    REQUIRE(event.toState == 0);
}

TEST_CASE("initFSM 使用非零初始狀態", "[fsm][init]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Active")
        .initialState("Active")
        .build();
    reg.emplace<FSMDefinition>(entity, def);

    initFSM(reg, entity, def);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.currentState == 1);
}

// ============================================================
// updateFSMSystem - Event Transitions
// ============================================================

TEST_CASE("updateFSMSystem 事件觸發狀態轉換", "[fsm][update]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .transition("Idle", "Walking", "StartWalk")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);

    // Clear initial state change event
    reg.clear<FSMStateChangeEvent>();

    // Push event
    auto& events = reg.get_or_emplace<FSMEventQueue>(entity);
    events.push("StartWalk");

    updateFSMSystem(reg, 0.016f);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.currentState == 1);
    REQUIRE(state.stateTime == Approx(0.016f));

    // Should emit state change event
    REQUIRE(reg.all_of<FSMStateChangeEvent>(entity));
    auto& changeEvent = reg.get<FSMStateChangeEvent>(entity);
    REQUIRE(changeEvent.fromState == 0);
    REQUIRE(changeEvent.toState == 1);
}

TEST_CASE("updateFSMSystem 無效事件不觸發轉換", "[fsm][update]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .transition("Idle", "Walking", "StartWalk")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);
    reg.clear<FSMStateChangeEvent>();

    auto& events = reg.get_or_emplace<FSMEventQueue>(entity);
    events.push("InvalidEvent");

    updateFSMSystem(reg, 0.016f);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.currentState == 0);
    REQUIRE_FALSE(reg.all_of<FSMStateChangeEvent>(entity));
}

TEST_CASE("updateFSMSystem 事件隊列在處理後清空", "[fsm][update]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);

    auto& events = reg.get_or_emplace<FSMEventQueue>(entity);
    events.push("Event1");
    events.push("Event2");
    REQUIRE(events.events.size() == 2);

    updateFSMSystem(reg, 0.016f);

    REQUIRE(events.empty());
}

TEST_CASE("updateFSMSystem minStateTime 限制轉換", "[fsm][update]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .build();
    def.eventTransitions.emplace_back(0, 1, "StartWalk", 0.5f);  // minStateTime = 0.5s
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);
    reg.clear<FSMStateChangeEvent>();

    auto& events = reg.get_or_emplace<FSMEventQueue>(entity);

    SECTION("stateTime < minStateTime: 不轉換") {
        events.push("StartWalk");
        updateFSMSystem(reg, 0.1f);  // stateTime = 0.1s < 0.5s

        auto& state = reg.get<FSMStateComponent>(entity);
        REQUIRE(state.currentState == 0);
    }

    SECTION("stateTime >= minStateTime: 轉換") {
        // Advance time first
        updateFSMSystem(reg, 0.5f);

        events.push("StartWalk");
        updateFSMSystem(reg, 0.016f);

        auto& state = reg.get<FSMStateComponent>(entity);
        REQUIRE(state.currentState == 1);
    }
}

// ============================================================
// updateFSMSystem - Timed Transitions
// ============================================================

TEST_CASE("updateFSMSystem 時間轉換", "[fsm][update][timed]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Attack")
        .state("Idle")
        .timedTransition("Attack", "Idle", 0.5f)
        .initialState("Attack")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);
    reg.clear<FSMStateChangeEvent>();

    SECTION("時間未到: 不轉換") {
        updateFSMSystem(reg, 0.3f);

        auto& state = reg.get<FSMStateComponent>(entity);
        REQUIRE(state.currentState == 0);
        REQUIRE_FALSE(reg.all_of<FSMStateChangeEvent>(entity));
    }

    SECTION("時間到達: 轉換") {
        updateFSMSystem(reg, 0.5f);

        auto& state = reg.get<FSMStateComponent>(entity);
        REQUIRE(state.currentState == 1);
        REQUIRE(reg.all_of<FSMStateChangeEvent>(entity));
    }

    SECTION("跨多幀累積時間") {
        updateFSMSystem(reg, 0.2f);
        updateFSMSystem(reg, 0.2f);
        REQUIRE(reg.get<FSMStateComponent>(entity).currentState == 0);

        updateFSMSystem(reg, 0.2f);  // Total: 0.6f >= 0.5f
        REQUIRE(reg.get<FSMStateComponent>(entity).currentState == 1);
    }
}

// ============================================================
// updateFSMSystem - Timer Updates
// ============================================================

TEST_CASE("updateFSMSystem 更新計時器", "[fsm][update][timer]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);

    updateFSMSystem(reg, 0.1f);
    updateFSMSystem(reg, 0.2f);
    updateFSMSystem(reg, 0.3f);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.stateTime == Approx(0.6f));
    REQUIRE(state.totalTime == Approx(0.6f));
}

TEST_CASE("updateFSMSystem 轉換後重置 stateTime", "[fsm][update][timer]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .transition("Idle", "Walking", "StartWalk")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);

    // Accumulate time in Idle state
    updateFSMSystem(reg, 1.0f);
    REQUIRE(reg.get<FSMStateComponent>(entity).stateTime == Approx(1.0f));

    // Trigger transition
    reg.get_or_emplace<FSMEventQueue>(entity).push("StartWalk");
    updateFSMSystem(reg, 0.1f);

    auto& state = reg.get<FSMStateComponent>(entity);
    REQUIRE(state.currentState == 1);
    REQUIRE(state.stateTime == Approx(0.1f));  // Reset after transition
    REQUIRE(state.totalTime == Approx(1.1f));  // Total keeps accumulating
}

// ============================================================
// updateFSMSystem - FSMStateChangeEvent Lifecycle
// ============================================================

TEST_CASE("updateFSMSystem 每幀清除舊的 FSMStateChangeEvent", "[fsm][update][event]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);

    // initFSM emits initial event
    REQUIRE(reg.all_of<FSMStateChangeEvent>(entity));

    // Next frame should clear it
    updateFSMSystem(reg, 0.016f);
    REQUIRE_FALSE(reg.all_of<FSMStateChangeEvent>(entity));
}

TEST_CASE("updateFSMSystem FSMStateChangeEvent 包含正確資訊", "[fsm][update][event]") {
    entt::registry reg;
    auto entity = reg.create();

    auto def = FSMDefinitionBuilder()
        .state("Idle")
        .state("Walking")
        .transition("Idle", "Walking", "StartWalk")
        .initialState("Idle")
        .build();
    reg.emplace<FSMDefinition>(entity, def);
    initFSM(reg, entity, def);
    reg.clear<FSMStateChangeEvent>();

    // Accumulate some time
    updateFSMSystem(reg, 0.5f);
    updateFSMSystem(reg, 0.3f);

    // Trigger transition
    reg.get_or_emplace<FSMEventQueue>(entity).push("StartWalk");
    updateFSMSystem(reg, 0.016f);

    REQUIRE(reg.all_of<FSMStateChangeEvent>(entity));
    auto& event = reg.get<FSMStateChangeEvent>(entity);
    REQUIRE(event.fromState == 0);
    REQUIRE(event.toState == 1);
    REQUIRE(event.previousStateTime == Approx(0.8f));
}

// ============================================================
// Multiple Entities
// ============================================================

TEST_CASE("updateFSMSystem 處理多個實體", "[fsm][update][multi]") {
    entt::registry reg;

    auto def1 = FSMDefinitionBuilder()
        .state("A").state("B")
        .transition("A", "B", "GoB")
        .initialState("A")
        .build();

    auto def2 = FSMDefinitionBuilder()
        .state("X").state("Y")
        .transition("X", "Y", "GoY")
        .initialState("X")
        .build();

    auto e1 = reg.create();
    reg.emplace<FSMDefinition>(e1, def1);
    initFSM(reg, e1, def1);

    auto e2 = reg.create();
    reg.emplace<FSMDefinition>(e2, def2);
    initFSM(reg, e2, def2);

    reg.clear<FSMStateChangeEvent>();

    // Only trigger e1
    reg.get_or_emplace<FSMEventQueue>(e1).push("GoB");

    updateFSMSystem(reg, 0.016f);

    REQUIRE(reg.get<FSMStateComponent>(e1).currentState == 1);
    REQUIRE(reg.get<FSMStateComponent>(e2).currentState == 0);
    REQUIRE(reg.all_of<FSMStateChangeEvent>(e1));
    REQUIRE_FALSE(reg.all_of<FSMStateChangeEvent>(e2));
}

// ============================================================
// Helper Functions
// ============================================================

TEST_CASE("sendFSMEvent 推送事件到隊列", "[fsm][helper]") {
    entt::registry reg;
    auto entity = reg.create();

    sendFSMEvent(reg, entity, "TestEvent");

    REQUIRE(reg.all_of<FSMEventQueue>(entity));
    auto& events = reg.get<FSMEventQueue>(entity);
    REQUIRE(events.events.size() == 1);
    REQUIRE(events.events[0] == "TestEvent");
}

TEST_CASE("broadcastFSMEvent 廣播事件到所有隊列", "[fsm][helper]") {
    entt::registry reg;

    auto e1 = reg.create();
    auto e2 = reg.create();
    auto e3 = reg.create();  // No FSMEventQueue

    reg.emplace<FSMEventQueue>(e1);
    reg.emplace<FSMEventQueue>(e2);

    broadcastFSMEvent(reg, "GlobalEvent");

    REQUIRE(reg.get<FSMEventQueue>(e1).events.size() == 1);
    REQUIRE(reg.get<FSMEventQueue>(e1).events[0] == "GlobalEvent");
    REQUIRE(reg.get<FSMEventQueue>(e2).events.size() == 1);
    REQUIRE(reg.get<FSMEventQueue>(e2).events[0] == "GlobalEvent");
    REQUIRE_FALSE(reg.all_of<FSMEventQueue>(e3));
}
