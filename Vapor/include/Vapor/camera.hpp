#pragma once
#include "glm/ext/matrix_clip_space.hpp"
#include <array>
#include <fmt/core.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

enum class FrustumPlane {
    FRUSTUM_LEFT = 0,
    FRUSTUM_RIGHT = 1,
    FRUSTUM_BOTTOM = 2,
    FRUSTUM_TOP = 3,
    FRUSTUM_NEAR = 4,
    FRUSTUM_FAR = 5,
};

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
    )
      : _eye(eye), _center(center), _up(up), _fov(fov), _aspect(aspect), _near(near), _far(far) {
    }

    glm::vec3 getEye() const {
        return _eye;
    }
    void setEye(const glm::vec3& position) {
        _eye = position;
        _isViewDirty = true;
        _isFrustumDirty = true;
    }

    glm::vec3 getCenter() const {
        return _center;
    }
    void setCenter(const glm::vec3& center) {
        _center = center;
        _isViewDirty = true;
        _isFrustumDirty = true;
    }

    void setLookAt(const glm::vec3& target) {
        setCenter(target);
    }

    glm::vec3 getForward() const {
        return glm::normalize(_center - _eye);
    }

    glm::mat4 getViewMatrix() {
        if (_isViewDirty) {
            _viewMatrix = glm::lookAt(_eye, _center, _up);
            _isViewDirty = false;
        }
        return _viewMatrix;
    };
    void setViewMatrix(const glm::mat4& viewMatrix) {
        _viewMatrix = viewMatrix;
        _isViewDirty = false;
    }

    glm::mat4 getProjMatrix() {
        if (_isProjDirty) {
            _projMatrix = glm::perspective(_fov, _aspect, _near, _far);
            _isProjDirty = false;
        }
        return _projMatrix;
    }
    void setProjectionMatrix(const glm::mat4& projMatrix) {
        _projMatrix = projMatrix;
        _isProjDirty = false;
    }

    std::array<glm::vec4, 6> getFrustumPlanes() {
        if (_isFrustumDirty) {
            glm::mat4 combo = getProjMatrix() * getViewMatrix();

            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_LEFT)] = glm::vec4(
                combo[0][3] + combo[0][0],
                combo[1][3] + combo[1][0],
                combo[2][3] + combo[2][0],
                combo[3][3] + combo[3][0]
            );
            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_RIGHT)] = glm::vec4(
                combo[0][3] - combo[0][0],
                combo[1][3] - combo[1][0],
                combo[2][3] - combo[2][0],
                combo[3][3] - combo[3][0]
            );
            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_BOTTOM)] = glm::vec4(
                combo[0][3] + combo[0][1],
                combo[1][3] + combo[1][1],
                combo[2][3] + combo[2][1],
                combo[3][3] + combo[3][1]
            );
            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_TOP)] = glm::vec4(
                combo[0][3] - combo[0][1],
                combo[1][3] - combo[1][1],
                combo[2][3] - combo[2][1],
                combo[3][3] - combo[3][1]
            );
            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_NEAR)] = glm::vec4(
                combo[0][3] + combo[0][2],
                combo[1][3] + combo[1][2],
                combo[2][3] + combo[2][2],
                combo[3][3] + combo[3][2]
            );
            _frustumPlanes[static_cast<size_t>(FrustumPlane::FRUSTUM_FAR)] = glm::vec4(
                combo[0][3] - combo[0][2],
                combo[1][3] - combo[1][2],
                combo[2][3] - combo[2][2],
                combo[3][3] - combo[3][2]
            );
            for (auto& plane : _frustumPlanes) {
                plane /= glm::length(glm::vec3(plane.x, plane.y, plane.z));
            }
            // Debug output
            // const char* names[] = {"L", "R", "B", "T", "N", "F"};
            // fmt::print("Frustum planes:\n");
            // for (int i = 0; i < 6; ++i) {
            //     const auto& plane = _frustumPlanes[i];
            //     fmt::print("{}: ({:.3f}, {:.3f}, {:.3f}, {:.3f})\n",
            //         names[i], plane.x, plane.y, plane.z, plane.w);
            // }

            _isFrustumDirty = false;
        }
        return _frustumPlanes;
    }

    void dolly(float offset);
    void truck(float offset);
    void pedestal(float offset);

    void pan(float angle);
    void tilt(float angle);
    void roll(float angle);

    void orbit(float angle);

    void updateAspectRatio(float aspect);

    float near() const {
        return _near;
    }
    float far() const {
        return _far;
    }

    bool isVisible(const glm::vec4& bsphere) {
        getFrustumPlanes();// also updates frustum planes
        glm::vec3 center = glm::vec3(bsphere);
        float radius = bsphere.w;
        for (const auto& plane : _frustumPlanes) {
            glm::vec3 normal = glm::vec3(plane.x, plane.y, plane.z);
            float dist = glm::dot(normal, center) + plane.w;
            if (dist < -radius) {
                return false;
            }
        }
        return true;
    }

    bool isVisible(const glm::vec3& min, const glm::vec3& max) {
        getFrustumPlanes();// also updates frustum planes
        for (const auto& plane : _frustumPlanes) {
            glm::vec3 normal = glm::vec3(plane.x, plane.y, plane.z);
            glm::vec3 farthest = max;
            glm::vec3 nearest = min;
            if (normal.x < 0) std::swap(farthest.x, nearest.x);
            if (normal.y < 0) std::swap(farthest.y, nearest.y);
            if (normal.z < 0) std::swap(farthest.z, nearest.z);
            float dist = glm::dot(normal, farthest) + plane.w;
            if (dist < 0) {
                return false;
            }
        }
        return true;
    }

private:
    glm::vec3 _eye;
    glm::vec3 _center;
    glm::vec3 _up;
    float _fov;
    float _aspect;
    float _near;
    float _far;

    glm::mat4 _viewMatrix;
    glm::mat4 _projMatrix;
    std::array<glm::vec4, 6> _frustumPlanes;

    bool _isViewDirty = true;
    bool _isProjDirty = true;
    bool _isFrustumDirty = true;
};
