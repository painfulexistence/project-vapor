#include "ecs_components.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Vapor {

glm::mat4 Transform::computeLocalMatrix() const {
    glm::mat4 mat = glm::translate(glm::mat4(1.0f), position);
    mat *= glm::mat4_cast(rotation);
    mat = glm::scale(mat, scale);
    return mat;
}

void Transform::setPosition(const glm::vec3& pos) {
    position = pos;
    isDirty = true;
}

void Transform::setRotation(const glm::quat& rot) {
    rotation = rot;
    isDirty = true;
}

void Transform::setRotation(const glm::vec3& eulerAngles) {
    rotation = glm::quat(eulerAngles);
    isDirty = true;
}

void Transform::setScale(const glm::vec3& scl) {
    if (scl.x != 0.0f && scl.y != 0.0f && scl.z != 0.0f) {
        scale = scl;
        isDirty = true;
    }
}

void Transform::translate(const glm::vec3& offset) {
    position += offset;
    isDirty = true;
}

void Transform::rotate(const glm::vec3& axis, float angle) {
    glm::quat deltaRot = glm::angleAxis(angle, glm::normalize(axis));
    rotation = deltaRot * rotation;
    isDirty = true;
}

void Transform::scaleBy(const glm::vec3& factor) {
    scale *= factor;
    isDirty = true;
}

} // namespace Vapor
