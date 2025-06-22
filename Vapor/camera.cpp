#include "camera.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/trigonometric.hpp>
#include <glm/gtx/rotate_vector.hpp>

void Camera::Dolly(float offset) {
    glm::vec3 dir = _center - _eye;
    _eye += offset * dir;
    _center = _eye + dir;
    _isViewDirty = true;
}

void Camera::Truck(float offset) {
    glm::vec3 dir = _center - _eye;
    _eye += offset * glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
    _center = _eye + dir;
    _isViewDirty = true;
}

void Camera::Pedestal(float offset) {
    _eye += glm::vec3(0.0f, offset, 0.0f);
    _center += glm::vec3(0.0f, offset, 0.0f);
    _isViewDirty = true;
}

void Camera::Pan(float radians) {
    glm::vec3 dir = _center - _eye;
    dir = glm::rotate(dir, radians, glm::vec3(0.0f, 1.0f, 0.0f));
    _center = _eye + dir;
    _isViewDirty = true;
}

void Camera::Tilt(float radians) {
    glm::vec3 dir = _center - _eye;
    glm::vec3 right = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
    dir = glm::rotate(dir, radians, right);
    if (std::abs(glm::dot(glm::normalize(dir), glm::vec3(0.0f, 1.0f, 0.0f))) < 1.0f) {
        _center = _eye + dir;
        _isViewDirty = true;
    }
}

void Camera::Roll(float radians) {
    glm::vec3 dir = _center - _eye;
    _up = glm::rotate(_up, radians, dir);
    _isViewDirty = true;
}

void Camera::Orbit(float radians) {
    glm::vec3 dir = _eye - _center;
    dir = glm::rotate(dir, radians, glm::vec3(0.0f, 1.0f, 0.0f));
    _eye = _center + dir;
    _isViewDirty = true;
}

void Camera::UpdateAspectRatio(float aspect) {
    _aspect = aspect;
    _isProjDirty = true;
}