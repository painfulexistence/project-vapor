/**
 * ActionManager Usage Example
 *
 * This example demonstrates how to use the ActionManager system
 * to create time-based actions and sequences.
 */

#include "Vapor/action_manager.hpp"
#include <iostream>
#include <memory>

using namespace Vapor;

int main() {
    // Create an ActionManager instance
    ActionManager actionManager;

    std::cout << "=== ActionManager Example ===\n\n";

    // Example 1: Simple delay action
    std::cout << "1. Creating a 1-second delay action...\n";
    auto delayAction = std::make_shared<DelayAction>(1.0f);
    actionManager.start(delayAction, "delay_test");

    // Example 2: Timed callback
    std::cout << "2. Creating a timed callback (0.5 seconds)...\n";
    auto timedCallback = std::make_shared<TimedCallbackAction>(0.5f, []() {
        std::cout << "   -> Timed callback executed after 0.5 seconds!\n";
    });
    actionManager.start(timedCallback, "callback_test");

    // Example 3: Timeline (sequence of actions)
    std::cout << "3. Creating a timeline with multiple actions...\n";
    auto timeline = std::make_shared<TimelineAction>();
    timeline->add(std::make_shared<CallbackAction>([]() {
        std::cout << "   -> Timeline: Step 1 - Starting\n";
    }));
    timeline->add(std::make_shared<DelayAction>(0.3f));
    timeline->add(std::make_shared<CallbackAction>([]() {
        std::cout << "   -> Timeline: Step 2 - After 0.3s delay\n";
    }));
    timeline->add(std::make_shared<DelayAction>(0.3f));
    timeline->add(std::make_shared<CallbackAction>([]() {
        std::cout << "   -> Timeline: Step 3 - After another 0.3s delay\n";
    }));
    actionManager.start(timeline, "timeline_test");

    // Example 4: Parallel actions
    std::cout << "4. Creating parallel actions...\n";
    auto parallel = std::make_shared<ParallelAction>();
    parallel->add(std::make_shared<TimedCallbackAction>(0.2f, []() {
        std::cout << "   -> Parallel: Fast action (0.2s) completed\n";
    }));
    parallel->add(std::make_shared<TimedCallbackAction>(0.4f, []() {
        std::cout << "   -> Parallel: Slow action (0.4s) completed\n";
    }));
    actionManager.start(parallel, "parallel_test");

    // Example 5: Update action with progress
    std::cout << "5. Creating an update action with progress tracking...\n";
    auto updateAction = std::make_shared<UpdateAction>(1.0f,
        [](float dt, float progress) {
            if (static_cast<int>(progress * 100) % 25 == 0) {
                std::cout << "   -> Progress: " << (progress * 100) << "%\n";
            }
        });
    actionManager.start(updateAction, "update_test");

    // Example 6: Repeat action
    std::cout << "6. Creating a repeat action (3 times)...\n";
    int repeatCount = 0;
    auto repeatedAction = std::make_shared<RepeatAction>(
        std::make_shared<TimedCallbackAction>(0.2f, [&repeatCount]() {
            repeatCount++;
            std::cout << "   -> Repeat: Execution #" << repeatCount << "\n";
        }),
        3  // Repeat 3 times
    );
    actionManager.start(repeatedAction, "repeat_test");

    // Simulate game loop
    std::cout << "\n=== Starting simulation (2 seconds) ===\n\n";
    const float dt = 0.016f;  // ~60 FPS
    float totalTime = 0.0f;
    const float maxTime = 2.0f;

    while (totalTime < maxTime) {
        actionManager.update(dt);
        totalTime += dt;

        // Small sleep to make output readable
        #ifdef _WIN32
        #include <windows.h>
        Sleep(16);
        #else
        #include <unistd.h>
        usleep(16000);
        #endif
    }

    std::cout << "\n=== Simulation complete ===\n";
    std::cout << "Active actions remaining: " << actionManager.getActionCount() << "\n";

    // Clean up
    actionManager.stopAll();

    return 0;
}
