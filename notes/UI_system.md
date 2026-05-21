# Modular UI System (ECS-Driven)

This system manages the lifecycle and state of RmlUi documents using an ECS architecture.

## Architecture Overview

- **`Vapor::Page`**: A C++ wrapper for RmlUi documents. Use this to bind logic to RML elements.
- **`UIStateComponent`**: A persistent component holding all registered pages and the current Menu Stack.
- **`PageSystem`**: The core engine system that handles loading, visibility transitions, and stack-based menu navigation.

## Usage Examples

For detailed, executable examples of how to use the UI system, please refer to the unit tests:
[tests/ui_system_test.cpp](file:///Users/loicchen/Desktop/code/cpp/project-vapor/tests/ui_system_test.cpp)

### Common Actions
```cpp
// Show/Hide a persistent overlay
PageSystem::show(registry, PageID::HUD);
PageSystem::hide(registry, PageID::HUD);

// Navigate through modal menus
PageSystem::push(registry, PageID::PauseMenu);
PageSystem::pop(registry);
```

## Cinematic Trigger Systems
- **`SubtitleQueueSystem`**: Handles timed subtitle sequences.
- **`ChapterTitleTriggerSystem`**: Handles one-shot title cards.
- **`ScrollTextQueueSystem`**: Handles teleprompter-style scrolling text.
