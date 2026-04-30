// Characterization tests for Scene Graph Node transform propagation.
// Tests cover local→world transform computation, dirty flag behaviour,
// and TRS decomposition. These lock CURRENT behaviour.
//
// NOTE: These tests do NOT link the full Vapor library — they test only
// the header-inline logic of Node (scene.hpp / glm operations).
// Physics, rendering, fluid volumes are excluded.

#define GLM_ENABLE_EXPERIMENTAL
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>
#include <array>

using Catch::Approx;

// ── Minimal Node stub (no Physics3D / CharacterController / VehicleController dependency) ──
// We replicate the pure-logic portions of Node to test them in isolation.
// This matches scene.hpp behaviour exactly.

struct MinimalNode {
    std::string name;
    std::vector<std::shared_ptr<MinimalNode>> children;
    glm::mat4 localTransform  = glm::identity<glm::mat4>();
    glm::mat4 worldTransform  = glm::identity<glm::mat4>();
    bool isTransformDirty     = true;

    glm::vec3 getLocalPosition() const { return glm::vec3(localTransform[3]); }

    glm::quat getLocalRotation() const {
        glm::mat3 r(
            glm::normalize(glm::vec3(localTransform[0])),
            glm::normalize(glm::vec3(localTransform[1])),
            glm::normalize(glm::vec3(localTransform[2]))
        );
        return glm::quat_cast(r);
    }

    glm::vec3 getLocalScale() const {
        return glm::vec3(
            glm::length(glm::vec3(localTransform[0])),
            glm::length(glm::vec3(localTransform[1])),
            glm::length(glm::vec3(localTransform[2]))
        );
    }

    glm::vec3 getWorldPosition() const { return glm::vec3(worldTransform[3]); }

    void setLocalPosition(const glm::vec3& pos) {
        glm::vec3 s = getLocalScale();
        glm::quat r = getLocalRotation();
        localTransform = glm::translate(glm::mat4(1.f), pos) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.f), s);
        isTransformDirty = true;
    }

    void setLocalRotation(const glm::quat& rot) {
        glm::vec3 p = getLocalPosition();
        glm::vec3 s = getLocalScale();
        localTransform = glm::translate(glm::mat4(1.f), p) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.f), s);
        isTransformDirty = true;
    }

    void setLocalScale(const glm::vec3& s) {
        if (s.x == 0.f || s.y == 0.f || s.z == 0.f) return;
        glm::vec3 p = getLocalPosition();
        glm::quat r = getLocalRotation();
        localTransform = glm::translate(glm::mat4(1.f), p) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.f), s);
        isTransformDirty = true;
    }

    void translate(const glm::vec3& offset) { setLocalPosition(getLocalPosition() + offset); }

    std::shared_ptr<MinimalNode> createChild(const std::string& n, const glm::mat4& t) {
        auto c = std::make_shared<MinimalNode>();
        c->name = n;
        c->localTransform = t;
        c->isTransformDirty = true;
        children.push_back(c);
        return c;
    }
};

// updateNode replicates Scene::updateNode logic exactly
static void updateNode(const std::shared_ptr<MinimalNode>& node, const glm::mat4& parentWorld, bool parentDirty = false) {
    bool dirty = node->isTransformDirty || parentDirty;
    if (dirty) {
        node->worldTransform = parentWorld * node->localTransform;
        node->isTransformDirty = false;
    }
    for (const auto& child : node->children) {
        updateNode(child, node->worldTransform, dirty);
    }
}

static void updateScene(std::vector<std::shared_ptr<MinimalNode>>& roots) {
    const glm::mat4 identity = glm::identity<glm::mat4>();
    for (const auto& node : roots) {
        updateNode(node, identity);
    }
}

// ============================================================
// Local transform decomposition
// ============================================================

TEST_CASE("Node getLocalPosition 從 localTransform 正確反解位置", "[scene][transform]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalPosition({3.0f, -1.5f, 7.0f});
    auto pos = node->getLocalPosition();
    REQUIRE(pos.x == Approx(3.0f));
    REQUIRE(pos.y == Approx(-1.5f));
    REQUIRE(pos.z == Approx(7.0f));
}

TEST_CASE("Node getLocalScale 從 localTransform 正確反解縮放", "[scene][transform]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalScale({2.0f, 3.0f, 0.5f});
    auto s = node->getLocalScale();
    REQUIRE(s.x == Approx(2.0f).epsilon(0.001f));
    REQUIRE(s.y == Approx(3.0f).epsilon(0.001f));
    REQUIRE(s.z == Approx(0.5f).epsilon(0.001f));
}

TEST_CASE("Node setLocalScale 零軸分量被忽略", "[scene][transform]") {
    // 鎖定行為：零 scale 呼叫被 early-return，localTransform 不變
    auto node = std::make_shared<MinimalNode>();
    node->setLocalPosition({1.0f, 2.0f, 3.0f});
    glm::mat4 before = node->localTransform;
    node->setLocalScale({0.0f, 1.0f, 1.0f});  // x=0，應被忽略
    REQUIRE(node->localTransform == before);
}

TEST_CASE("Node setLocalPosition 保持現有 rotation 和 scale", "[scene][transform]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalScale({2.0f, 2.0f, 2.0f});
    node->setLocalPosition({5.0f, 0.0f, 0.0f});

    auto s = node->getLocalScale();
    REQUIRE(s.x == Approx(2.0f).epsilon(0.001f));
    REQUIRE(s.y == Approx(2.0f).epsilon(0.001f));
    REQUIRE(s.z == Approx(2.0f).epsilon(0.001f));
}

// ============================================================
// Dirty flag
// ============================================================

TEST_CASE("Node 新建時 isTransformDirty = true", "[scene][dirty]") {
    auto node = std::make_shared<MinimalNode>();
    REQUIRE(node->isTransformDirty == true);
}

TEST_CASE("updateNode 清除 dirty flag", "[scene][dirty]") {
    auto node = std::make_shared<MinimalNode>();
    REQUIRE(node->isTransformDirty == true);
    updateNode(node, glm::identity<glm::mat4>());
    REQUIRE(node->isTransformDirty == false);
}

TEST_CASE("setLocalPosition 再次標記為 dirty", "[scene][dirty]") {
    auto node = std::make_shared<MinimalNode>();
    updateNode(node, glm::identity<glm::mat4>());
    REQUIRE(node->isTransformDirty == false);

    node->setLocalPosition({1.0f, 0.0f, 0.0f});
    REQUIRE(node->isTransformDirty == true);
}

TEST_CASE("updateNode 在 dirty=false 時不重新計算 worldTransform", "[scene][dirty]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalPosition({1.0f, 0.0f, 0.0f});
    updateNode(node, glm::identity<glm::mat4>());
    REQUIRE(node->isTransformDirty == false);

    // 手動污染 worldTransform 後不更新（dirty=false）
    glm::mat4 fakeWorld = glm::translate(glm::mat4(1.f), {99.f, 99.f, 99.f});
    node->worldTransform = fakeWorld;

    updateNode(node, glm::identity<glm::mat4>());
    // dirty=false: worldTransform is NOT recomputed and stays as fakeWorld.
    // WARNING: any children of this node will inherit the stale fakeWorld as their
    // parentWorld — this is faithfully reproduced from production scene.cpp:updateNode.
    REQUIRE(node->worldTransform == fakeWorld);
}

// ============================================================
// World transform 傳播
// ============================================================

TEST_CASE("根節點 worldTransform = parentWorld * localTransform", "[scene][world]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalPosition({5.0f, 0.0f, 0.0f});

    glm::mat4 parent = glm::translate(glm::mat4(1.f), {10.0f, 0.0f, 0.0f});
    updateNode(node, parent);

    auto worldPos = node->getWorldPosition();
    REQUIRE(worldPos.x == Approx(15.0f));  // 10 + 5
    REQUIRE(worldPos.y == Approx(0.0f));
    REQUIRE(worldPos.z == Approx(0.0f));
}

TEST_CASE("子節點 worldTransform 由父節點 worldTransform 決定", "[scene][world]") {
    auto parent = std::make_shared<MinimalNode>();
    parent->setLocalPosition({10.0f, 0.0f, 0.0f});

    auto child = parent->createChild("child",
        glm::translate(glm::mat4(1.f), {5.0f, 0.0f, 0.0f}));

    updateNode(parent, glm::identity<glm::mat4>());

    auto childWorldPos = child->getWorldPosition();
    REQUIRE(childWorldPos.x == Approx(15.0f));
    REQUIRE(childWorldPos.y == Approx(0.0f));
    REQUIRE(childWorldPos.z == Approx(0.0f));
}

TEST_CASE("三層節點樹 world transform 正確傳遞", "[scene][world]") {
    auto root = std::make_shared<MinimalNode>();
    root->setLocalPosition({1.0f, 0.0f, 0.0f});

    auto mid = root->createChild("mid", glm::translate(glm::mat4(1.f), {2.0f, 0.0f, 0.0f}));
    auto leaf = mid->createChild("leaf", glm::translate(glm::mat4(1.f), {3.0f, 0.0f, 0.0f}));

    updateNode(root, glm::identity<glm::mat4>());

    REQUIRE(root->getWorldPosition().x == Approx(1.0f));
    REQUIRE(mid->getWorldPosition().x  == Approx(3.0f));   // 1+2
    REQUIRE(leaf->getWorldPosition().x == Approx(6.0f));   // 1+2+3
}

TEST_CASE("父節點移動後子節點 dirty 傳播（需重新呼叫 update）", "[scene][world]") {
    auto parent = std::make_shared<MinimalNode>();
    auto child  = parent->createChild("c", glm::identity<glm::mat4>());

    updateNode(parent, glm::identity<glm::mat4>());
    REQUIRE(parent->isTransformDirty == false);
    REQUIRE(child->isTransformDirty == false);

    // 移動父節點
    parent->setLocalPosition({5.0f, 0.0f, 0.0f});
    REQUIRE(parent->isTransformDirty == true);
    // 子節點的 dirty flag 本身不會被自動設定（由呼叫者負責觸發 update）
    // 鎖定此行為：子節點 dirty 是 false，但 worldTransform 過期了
    REQUIRE(child->isTransformDirty == false);

    // 重新 update：父節點 dirty，子節點因此強制更新
    // 注意：子節點的 updateNode 會用父節點新的 worldTransform 更新子節點
    updateNode(parent, glm::identity<glm::mat4>());
    REQUIRE(child->getWorldPosition().x == Approx(5.0f));
}

TEST_CASE("Node::translate 累加位移", "[scene][transform]") {
    auto node = std::make_shared<MinimalNode>();
    node->setLocalPosition({1.0f, 0.0f, 0.0f});
    node->translate({2.0f, 3.0f, 0.0f});

    auto pos = node->getLocalPosition();
    REQUIRE(pos.x == Approx(3.0f));
    REQUIRE(pos.y == Approx(3.0f));
}

// ============================================================
// Scene::findNode 行為
// ============================================================

TEST_CASE("findNodeInHierarchy 找到目標節點", "[scene][find]") {
    // 複製 Scene::findNodeInHierarchy 邏輯用於測試
    std::function<std::shared_ptr<MinimalNode>(const std::string&, const std::shared_ptr<MinimalNode>&)> findInHierarchy;
    findInHierarchy = [&](const std::string& name, const std::shared_ptr<MinimalNode>& node) -> std::shared_ptr<MinimalNode> {
        if (node->name == name) return node;
        for (const auto& child : node->children) {
            auto r = findInHierarchy(name, child);
            if (r) return r;
        }
        return nullptr;
    };

    auto root = std::make_shared<MinimalNode>();
    root->name = "root";
    auto child = root->createChild("target", glm::identity<glm::mat4>());
    child->createChild("leaf", glm::identity<glm::mat4>());

    auto found = findInHierarchy("target", root);
    REQUIRE(found != nullptr);
    REQUIRE(found->name == "target");
}

TEST_CASE("findNodeInHierarchy 找不到時回傳 nullptr", "[scene][find]") {
    std::function<std::shared_ptr<MinimalNode>(const std::string&, const std::shared_ptr<MinimalNode>&)> findInHierarchy;
    findInHierarchy = [&](const std::string& name, const std::shared_ptr<MinimalNode>& node) -> std::shared_ptr<MinimalNode> {
        if (node->name == name) return node;
        for (const auto& child : node->children) {
            auto r = findInHierarchy(name, child);
            if (r) return r;
        }
        return nullptr;
    };

    auto root = std::make_shared<MinimalNode>();
    root->name = "root";

    auto found = findInHierarchy("nonexistent", root);
    REQUIRE(found == nullptr);
}
