// Characterization tests for the Action System (Easing, Timer, ActionManager).
// These lock CURRENT behaviour — do not change expected values without understanding the impact.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Vapor/action_manager.hpp>
#include <vector>

using namespace Vapor;
using Catch::Approx;

// ============================================================
// Easing Functions
// ============================================================

TEST_CASE("Easing::Linear 邊界值", "[easing][linear]") {
    REQUIRE(Easing::Linear(0.0f) == Approx(0.0f));
    REQUIRE(Easing::Linear(1.0f) == Approx(1.0f));
    REQUIRE(Easing::Linear(0.5f) == Approx(0.5f));
    REQUIRE(Easing::Linear(0.25f) == Approx(0.25f));
}

TEST_CASE("Easing::InQuad 邊界值與中點", "[easing][quad]") {
    REQUIRE(Easing::InQuad(0.0f) == Approx(0.0f));
    REQUIRE(Easing::InQuad(1.0f) == Approx(1.0f));
    REQUIRE(Easing::InQuad(0.5f) == Approx(0.25f));  // 0.5^2
}

TEST_CASE("Easing::OutQuad 邊界值與中點", "[easing][quad]") {
    REQUIRE(Easing::OutQuad(0.0f) == Approx(0.0f));
    REQUIRE(Easing::OutQuad(1.0f) == Approx(1.0f));
    REQUIRE(Easing::OutQuad(0.5f) == Approx(0.75f));  // 0.5*(2-0.5)
}

TEST_CASE("Easing::InOutQuad 對稱性與邊界", "[easing][quad]") {
    REQUIRE(Easing::InOutQuad(0.0f) == Approx(0.0f));
    REQUIRE(Easing::InOutQuad(1.0f) == Approx(1.0f));
    REQUIRE(Easing::InOutQuad(0.5f) == Approx(0.5f));
    // 前半段用 InQuad，後半段用 OutQuad——t=0.25 應小於 0.5
    REQUIRE(Easing::InOutQuad(0.25f) < 0.5f);
    REQUIRE(Easing::InOutQuad(0.75f) > 0.5f);
}

TEST_CASE("Easing::InCubic 邊界值", "[easing][cubic]") {
    REQUIRE(Easing::InCubic(0.0f) == Approx(0.0f));
    REQUIRE(Easing::InCubic(1.0f) == Approx(1.0f));
    REQUIRE(Easing::InCubic(0.5f) == Approx(0.125f));  // 0.5^3
}

TEST_CASE("Easing::OutCubic 邊界值", "[easing][cubic]") {
    REQUIRE(Easing::OutCubic(0.0f) == Approx(0.0f));
    REQUIRE(Easing::OutCubic(1.0f) == Approx(1.0f));
}

TEST_CASE("Easing::OutBack 超射行為（值會超過 1.0）", "[easing][back]") {
    // OutBack 刻意超射——鎖定這個「Bug-like」行為是特徵化測試的精髓
    REQUIRE(Easing::OutBack(0.0f) == Approx(0.0f));
    REQUIRE(Easing::OutBack(1.0f) == Approx(1.0f));
    // 中途某點會超過 1.0（overshoot）
    float maxVal = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        maxVal = std::max(maxVal, Easing::OutBack(i / 100.0f));
    }
    REQUIRE(maxVal > 1.0f);  // 確認超射行為存在
}

// ============================================================
// Timer
// ============================================================

TEST_CASE("Timer 初始狀態", "[timer]") {
    Timer t(1.0f);
    REQUIRE(t.getElapsed() == Approx(0.0f));
    REQUIRE(t.getDuration() == Approx(1.0f));
    REQUIRE(t.getProgress() == Approx(0.0f));
    REQUIRE_FALSE(t.isComplete());
}

TEST_CASE("Timer::update 回傳 true 當剛好完成", "[timer]") {
    Timer t(0.5f);
    bool done = t.update(0.5f);
    REQUIRE(done == true);
    REQUIRE(t.isComplete());
}

TEST_CASE("Timer::update 不超過 duration 不回傳 true", "[timer]") {
    Timer t(1.0f);
    bool done = t.update(0.3f);
    REQUIRE(done == false);
    REQUIRE_FALSE(t.isComplete());
}

TEST_CASE("Timer::getProgress 鉗制在 1.0", "[timer]") {
    Timer t(1.0f);
    t.update(0.5f);
    REQUIRE(t.getProgress() == Approx(0.5f));
    t.update(2.0f);  // 超過 duration
    REQUIRE(t.getProgress() == Approx(1.0f));
}

TEST_CASE("Timer duration=0 時 getProgress 回傳 1.0", "[timer]") {
    // 零時長 timer 鎖定行為：progress 直接是 1.0，不除以 0
    Timer t(0.0f);
    REQUIRE(t.getProgress() == Approx(1.0f));
}

TEST_CASE("Timer::reset 重置計時", "[timer]") {
    Timer t(1.0f);
    t.update(0.8f);
    t.reset();
    REQUIRE(t.getElapsed() == Approx(0.0f));
    REQUIRE_FALSE(t.isComplete());
}

TEST_CASE("Timer::reset 可以改變 duration", "[timer]") {
    Timer t(1.0f);
    t.reset(2.0f);
    REQUIRE(t.getDuration() == Approx(2.0f));
    REQUIRE(t.getElapsed() == Approx(0.0f));
}

TEST_CASE("Timer::update 完成後繼續 update 仍回傳 true", "[timer]") {
    // 鎖定：已完成的 timer 再 update 不會重置，持續回傳 true
    Timer t(0.1f);
    t.update(0.5f);
    REQUIRE(t.isComplete());
    bool stillDone = t.update(0.1f);
    REQUIRE(stillDone == true);
}

// ============================================================
// DelayAction
// ============================================================

TEST_CASE("DelayAction 在 duration 後完成", "[action][delay]") {
    auto action = std::make_shared<DelayAction>(0.5f);
    action->onStart();
    REQUIRE_FALSE(action->isDone());
    action->update(0.3f);
    REQUIRE_FALSE(action->isDone());
    action->update(0.3f);  // 累計 0.6 >= 0.5
    REQUIRE(action->isDone());
}

TEST_CASE("DelayAction onStart 重置計時器", "[action][delay]") {
    auto action = std::make_shared<DelayAction>(0.5f);
    action->onStart();
    action->update(0.4f);
    REQUIRE_FALSE(action->isDone());
    action->onStart();  // reset
    action->update(0.4f);
    REQUIRE_FALSE(action->isDone());  // 重新計時，尚未完成
}

// ============================================================
// CallbackAction
// ============================================================

TEST_CASE("CallbackAction 在 onStart 時立即執行 callback", "[action][callback]") {
    int count = 0;
    auto action = std::make_shared<CallbackAction>([&count]() { count++; });
    REQUIRE(count == 0);
    action->onStart();
    REQUIRE(count == 1);
    REQUIRE(action->isDone());  // CallbackAction 在 onStart 後立即完成
}

TEST_CASE("CallbackAction update 不再次執行 callback", "[action][callback]") {
    int count = 0;
    auto action = std::make_shared<CallbackAction>([&count]() { count++; });
    action->onStart();
    action->update(1.0f);
    action->update(1.0f);
    REQUIRE(count == 1);  // 只執行一次
}

// ============================================================
// TimedCallbackAction
// ============================================================

TEST_CASE("TimedCallbackAction 在 delay 後執行 callback", "[action][timed_callback]") {
    int count = 0;
    auto action = std::make_shared<TimedCallbackAction>(0.5f, [&count]() { count++; });
    action->onStart();
    REQUIRE(count == 0);
    action->update(0.3f);
    REQUIRE(count == 0);
    action->update(0.3f);
    REQUIRE(count == 1);
    REQUIRE(action->isDone());
}

TEST_CASE("TimedCallbackAction duration=0 在 onStart 立即執行", "[action][timed_callback]") {
    int count = 0;
    auto action = std::make_shared<TimedCallbackAction>(0.0f, [&count]() { count++; });
    action->onStart();
    REQUIRE(count == 1);
    REQUIRE(action->isDone());
}

// ============================================================
// UpdateAction
// ============================================================

TEST_CASE("UpdateAction 在 duration 期間每幀呼叫 updateFunc", "[action][update]") {
    std::vector<float> progresses;
    auto action = std::make_shared<UpdateAction>(1.0f, [&](float dt, float progress) {
        progresses.push_back(progress);
    });
    action->onStart();
    action->update(0.25f);
    action->update(0.25f);
    action->update(0.25f);
    action->update(0.25f);

    REQUIRE(progresses.size() == 4);
    REQUIRE_FALSE(progresses.empty());
    // 最後一次 progress 應 >= 1.0（timer 鉗制）
    REQUIRE(progresses.back() == Approx(1.0f));
    REQUIRE(action->isDone());
}

// ============================================================
// TimelineAction（序列）
// ============================================================

TEST_CASE("TimelineAction 依序執行子動作", "[action][timeline]") {
    std::vector<int> order;
    auto timeline = std::make_shared<TimelineAction>();
    timeline->add(std::make_shared<CallbackAction>([&]() { order.push_back(1); }));
    timeline->add(std::make_shared<DelayAction>(0.5f));
    timeline->add(std::make_shared<CallbackAction>([&]() { order.push_back(2); }));

    timeline->onStart();
    // CallbackAction 立即完成，第二個 DelayAction 開始
    REQUIRE(order == std::vector<int>{1});
    REQUIRE_FALSE(timeline->isDone());

    // 推進超過 delay
    timeline->update(0.6f);
    REQUIRE(order == std::vector<int>{1, 2});
    REQUIRE(timeline->isDone());
}

TEST_CASE("TimelineAction 空序列立即完成", "[action][timeline]") {
    auto timeline = std::make_shared<TimelineAction>();
    timeline->onStart();
    REQUIRE(timeline->isDone());
}

// ============================================================
// ParallelAction（並行）
// ============================================================

TEST_CASE("ParallelAction 同時啟動所有子動作", "[action][parallel]") {
    std::vector<int> started;
    auto parallel = std::make_shared<ParallelAction>();
    parallel->add(std::make_shared<CallbackAction>([&]() { started.push_back(1); }));
    parallel->add(std::make_shared<CallbackAction>([&]() { started.push_back(2); }));

    parallel->onStart();
    // 兩個 CallbackAction 都在 onStart 時被呼叫
    REQUIRE(started.size() == 2);
}

TEST_CASE("ParallelAction 等待所有子動作完成才結束", "[action][parallel]") {
    auto parallel = std::make_shared<ParallelAction>();
    parallel->add(std::make_shared<DelayAction>(0.3f));
    parallel->add(std::make_shared<DelayAction>(0.7f));

    parallel->onStart();
    parallel->update(0.4f);
    REQUIRE_FALSE(parallel->isDone());  // 0.7s 的那個還沒完
    parallel->update(0.4f);
    REQUIRE(parallel->isDone());
}

TEST_CASE("ParallelAction 空時立即完成", "[action][parallel]") {
    auto parallel = std::make_shared<ParallelAction>();
    parallel->onStart();
    REQUIRE(parallel->isDone());
}

// ============================================================
// RepeatAction
// ============================================================

TEST_CASE("RepeatAction 重複指定次數", "[action][repeat]") {
    int count = 0;
    auto inner = std::make_shared<CallbackAction>([&]() { count++; });
    auto repeat = std::make_shared<RepeatAction>(inner, 3);

    // Execution trace (CallbackAction finishes in onStart, so each update drives one cycle):
    //   onStart()     → inner.onStart() fires callback (count=1), inner.isDone()=true
    //   update(0) #1  → inner already done; m_currentCount→1 (<3): reset+onStart → count=2
    //   update(0) #2  → inner done; m_currentCount→2 (<3): reset+onStart → count=3
    //   update(0) #3  → inner done; m_currentCount→3 (==3): m_finished=true
    repeat->onStart();
    repeat->update(0.0f);
    repeat->update(0.0f);
    repeat->update(0.0f);  // third cycle completes, RepeatAction finishes

    REQUIRE(count == 3);
    REQUIRE(repeat->isDone());
}

TEST_CASE("RepeatAction count=-1 不自動結束", "[action][repeat]") {
    int count = 0;
    auto inner = std::make_shared<DelayAction>(0.1f);
    auto repeat = std::make_shared<RepeatAction>(inner, -1);

    repeat->onStart();
    for (int i = 0; i < 100; ++i) {
        repeat->update(0.15f);  // 每次超過 delay，inner 重置
    }
    REQUIRE_FALSE(repeat->isDone());  // 永不結束
}

// ============================================================
// ActionManager
// ============================================================

TEST_CASE("ActionManager::start 後 getActionCount 增加", "[action_manager]") {
    ActionManager mgr;
    REQUIRE(mgr.getActionCount() == 0);
    mgr.start(std::make_shared<DelayAction>(1.0f));
    REQUIRE(mgr.getActionCount() == 1);
}

TEST_CASE("ActionManager::update 完成的 action 自動移除", "[action_manager]") {
    ActionManager mgr;
    mgr.start(std::make_shared<DelayAction>(0.5f));
    REQUIRE(mgr.getActionCount() == 1);
    mgr.update(1.0f);
    REQUIRE(mgr.getActionCount() == 0);
}

TEST_CASE("ActionManager::start 同一個 action 不重複加入", "[action_manager]") {
    ActionManager mgr;
    auto action = std::make_shared<DelayAction>(1.0f);
    mgr.start(action);
    mgr.start(action);  // 重複 start
    REQUIRE(mgr.getActionCount() == 1);
}

TEST_CASE("ActionManager tag 系統：hasTag / stopByTag", "[action_manager][tag]") {
    ActionManager mgr;
    mgr.start(std::make_shared<DelayAction>(10.0f), "anim");
    mgr.start(std::make_shared<DelayAction>(10.0f), "anim");

    REQUIRE(mgr.hasTag("anim"));
    REQUIRE(mgr.getActionCount() == 2);

    mgr.stopByTag("anim");
    REQUIRE_FALSE(mgr.hasTag("anim"));
    REQUIRE(mgr.getActionCount() == 0);
}

TEST_CASE("ActionManager::stopAll 清除所有 action 和 tag", "[action_manager]") {
    ActionManager mgr;
    mgr.start(std::make_shared<DelayAction>(10.0f), "a");
    mgr.start(std::make_shared<DelayAction>(10.0f), "b");
    REQUIRE(mgr.getActionCount() == 2);

    mgr.stopAll();
    REQUIRE(mgr.getActionCount() == 0);
    REQUIRE_FALSE(mgr.hasTag("a"));
    REQUIRE_FALSE(mgr.hasTag("b"));
}

TEST_CASE("ActionManager::start nullptr 安全處理", "[action_manager]") {
    ActionManager mgr;
    auto result = mgr.start(nullptr);
    REQUIRE(result == nullptr);
    REQUIRE(mgr.getActionCount() == 0);
}

TEST_CASE("ActionManager::getActionsByTag 回傳正確集合", "[action_manager][tag]") {
    ActionManager mgr;
    auto a1 = std::make_shared<DelayAction>(10.0f);
    auto a2 = std::make_shared<DelayAction>(10.0f);
    mgr.start(a1, "group");
    mgr.start(a2, "group");

    auto actions = mgr.getActionsByTag("group");
    REQUIRE(actions.size() == 2);
}

TEST_CASE("ActionManager 無 tag 的 action 在 stopByTag 後不受影響", "[action_manager][tag]") {
    ActionManager mgr;
    mgr.start(std::make_shared<DelayAction>(10.0f));         // no tag
    mgr.start(std::make_shared<DelayAction>(10.0f), "anim"); // tagged

    mgr.stopByTag("anim");
    REQUIRE(mgr.getActionCount() == 1);  // 沒有 tag 的那個仍存在
}
