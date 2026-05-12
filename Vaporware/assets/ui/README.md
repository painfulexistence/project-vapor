# RmlUi Integration Guide

## Overview

RmlUi has been integrated into AtmosphericEngine for creating game UI elements such as HUD, dialogs, and inventory screens.

## Usage in Your Game

### 1. Initialize RmlUi in your Application

In your application's `OnInit()` method:

```cpp
#include "rmlui_manager.hpp"

void MyGame::OnInit() {
    auto windowSize = GetWindow()->GetFramebufferSize();
    RmlUiManager::Get()->Initialize(windowSize.width, windowSize.height);
}
```

### 2. Load UI Documents

In your `OnLoad()` method or whenever you need to show UI:

```cpp
// Load HUD
auto hud = RmlUiManager::Get()->LoadDocument("assets/ui/hud.rml");
if (hud) {
    hud->Show();
}

// Load dialog (initially hidden)
auto dialog = RmlUiManager::Get()->LoadDocument("assets/ui/dialog.rml");

// Load inventory
auto inventory = RmlUiManager::Get()->LoadDocument("assets/ui/inventory.rml");
```

### 3. Update RmlUi Each Frame

In your `OnUpdate()` method:

```cpp
void MyGame::OnUpdate(float dt, float time) {
    RmlUiManager::Get()->Update(dt);
    // ... your game logic
}
```

### 4. Render RmlUi

RmlUi should be rendered AFTER the main 3D scene. This is typically done in GameLayer:

```cpp
void GameLayer::OnRender(float dt) {
    _app->GetGraphicsServer()->Render(dt);

    // Render RmlUi UI on top
    RmlUiManager::Get()->Render();
}
```

### 5. Handle Window Resize

If your window can be resized, handle it:

```cpp
void MyGame::OnResize(int width, int height) {
    RmlUiManager::Get()->OnResize(width, height);
}
```

## Available UI Templates

- **hud.rml** - Game HUD with health bar, compass, and objectives
- **dialog.rml** - Dialog/conversation interface
- **inventory.rml** - Inventory/backpack system with grid layout
- **style.rcss** - Global UI styles

## Customizing UI

### Modifying Existing Templates

Edit the `.rml` files to change layout and structure. Edit `.rcss` files to change styling.

### Creating New UI

Create new `.rml` files following the RmlUi documentation:
https://mikke89.github.io/RmlUiDoc/

### Dynamic Content

Access and modify UI elements from C++:

```cpp
auto context = RmlUiManager::Get()->GetContext();
auto document = context->GetDocument("hud");
if (document) {
    auto element = document->GetElementById("health-fill");
    if (element) {
        // Update health bar width based on player health
        float healthPercent = playerHealth / maxHealth;
        element->SetProperty("width", Rml::String(std::to_string(healthPercent * 100) + "%"));
    }
}
```

## Input Handling

Input can be forwarded to RmlUi for interactive UI elements:

```cpp
// Mouse events
RmlUiManager::Get()->ProcessMouseMove(x, y, modifiers);
RmlUiManager::Get()->ProcessMouseButtonDown(button, modifiers);
RmlUiManager::Get()->ProcessMouseButtonUp(button, modifiers);

// Keyboard events
RmlUiManager::Get()->ProcessKeyDown(key, modifiers);
RmlUiManager::Get()->ProcessKeyUp(key, modifiers);
```

## Showing/Hiding UI

```cpp
// Show a document
RmlUiManager::Get()->ShowDocument("dialog");

// Hide a document
RmlUiManager::Get()->HideDocument("inventory");

// Or use the document directly
auto inventory = context->GetDocument("inventory");
if (inventory) {
    inventory->Show();  // or Hide()
}
```

## Notes

- RmlUi uses its own coordinate system with "dp" (density-independent pixels) units
- UI is rendered with alpha blending, so transparent elements work correctly
- The RmlUi debugger is enabled in debug builds (press F8 to toggle)
- Font files should be placed in `assets/fonts/` directory
