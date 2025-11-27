#include "Vapor/action_manager.hpp"
#include <algorithm>

namespace Vapor {

// ============================================================
// ActionManager Implementation
// ============================================================

std::shared_ptr<Action> ActionManager::start(std::shared_ptr<Action> action, const std::string& tag) {
    if (!action) {
        return nullptr;
    }

    // Add to active actions list
    auto it = std::find(m_actions.begin(), m_actions.end(), action);
    if (it == m_actions.end()) {
        m_actions.push_back(action);
        action->onStart();
    }

    // Register tag if provided
    if (!tag.empty()) {
        m_actionTags[action].insert(tag);
        m_tagActions[tag].insert(action);
    }

    return action;
}

void ActionManager::stop(const std::shared_ptr<Action>& action) {
    if (!action) {
        return;
    }

    removeAction(action);
    action->finish();
}

void ActionManager::stopByTag(const std::string& tag) {
    if (tag.empty()) {
        return;
    }

    auto it = m_tagActions.find(tag);
    if (it == m_tagActions.end()) {
        return;
    }

    // Create a copy to avoid modification during iteration
    auto actionsToStop = it->second;
    for (const auto& action : actionsToStop) {
        stop(action);
    }
}

void ActionManager::stopAll() {
    // Clear tag registry
    m_actionTags.clear();
    m_tagActions.clear();

    // Mark all actions as finished
    for (auto& action : m_actions) {
        action->finish();
    }

    // Clear action list
    m_actions.clear();
}

bool ActionManager::hasTag(const std::string& tag) const {
    auto it = m_tagActions.find(tag);
    return it != m_tagActions.end() && !it->second.empty();
}

std::vector<std::shared_ptr<Action>> ActionManager::getActionsByTag(const std::string& tag) const {
    auto it = m_tagActions.find(tag);
    if (it == m_tagActions.end()) {
        return {};
    }

    return std::vector<std::shared_ptr<Action>>(it->second.begin(), it->second.end());
}

void ActionManager::update(float dt) {
    // Iterate over a copy to safely remove during iteration
    auto actionsCopy = m_actions;

    for (const auto& action : actionsCopy) {
        if (!action) {
            continue;
        }

        action->update(dt);

        if (action->isDone()) {
            removeAction(action);
        }
    }
}

void ActionManager::removeAction(const std::shared_ptr<Action>& action) {
    // Remove from actions list
    auto it = std::find(m_actions.begin(), m_actions.end(), action);
    if (it != m_actions.end()) {
        m_actions.erase(it);
    }

    // Remove from tag registry
    auto tagIt = m_actionTags.find(action);
    if (tagIt != m_actionTags.end()) {
        for (const auto& tag : tagIt->second) {
            auto& tagSet = m_tagActions[tag];
            tagSet.erase(action);

            // Remove empty tag entries
            if (tagSet.empty()) {
                m_tagActions.erase(tag);
            }
        }
        m_actionTags.erase(tagIt);
    }
}

} // namespace Vapor
