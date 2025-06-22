#pragma once
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include "glm/ext/matrix_clip_space.hpp"


class Camera {
public:
    Camera(
        glm::vec3 eye = glm::vec3(0.0f),
        glm::vec3 center = glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f),
        float fov = glm::radians(45.0f),
        float aspect = 1.0f,
        float near = 0.1f,
        float far = 500.0f
    ) : _eye(eye), _center(center), _up(up), _fov(fov), _aspect(aspect), _near(near), _far(far) {}

    glm::vec3 GetEye() const { return _eye; }
    void SetEye(const glm::vec3& position) { _eye = position; }

    glm::mat4 GetViewMatrix() {
        if (_isViewDirty) {
            _viewMatrix = glm::lookAt(_eye, _center, _up);
            _isViewDirty = false;
        }
        return _viewMatrix;
    };
    glm::mat4 GetProjMatrix() {
        if (_isProjDirty) {
            _projMatrix = glm::perspective(_fov, _aspect, _near, _far);
            _isProjDirty = false;
        }
        return _projMatrix;
    }

    void Dolly(float offset);
    void Truck(float offset);
    void Pedestal(float offset);

    void Pan(float angle);
    void Tilt(float angle);
    void Roll(float angle);

    void Orbit(float angle);

    void UpdateAspectRatio(float aspect);

private:
    glm::vec3 _eye;
    glm::vec3 _center;
    glm::vec3 _up;
    float _fov;
    float _aspect;
    float _near;
    float _far;
    bool _isViewDirty = true;
    bool _isProjDirty = true;
    glm::mat4 _viewMatrix;
    glm::mat4 _projMatrix;
};
