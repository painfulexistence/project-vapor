find_package(Catch2 3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Strict warnings for all test targets — keep tests warning-free
set(TEST_WARNING_FLAGS
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wshadow -Wno-unused-parameter>
)

# ── Action System tests ─────────────────────────────────────────────────────
add_executable(test_action_system
    action_system_safeguard_test.cpp
)
target_link_libraries(test_action_system PRIVATE
    Vapor
    Catch2::Catch2WithMain
    glm::glm
)
target_compile_features(test_action_system PRIVATE cxx_std_20)
target_compile_options(test_action_system PRIVATE ${TEST_WARNING_FLAGS})

# ── Node / Scene transform tests (pure logic, no GPU/Physics) ──────────────
add_executable(test_scene_transform
    scene_transform_safeguard_test.cpp
)
target_include_directories(test_scene_transform PRIVATE
    ${CMAKE_SOURCE_DIR}/Vapor/include
    ${CMAKE_SOURCE_DIR}/Vapor/include/Vapor
)
target_link_libraries(test_scene_transform PRIVATE
    Catch2::Catch2WithMain
    glm::glm
)
target_compile_features(test_scene_transform PRIVATE cxx_std_20)
target_compile_options(test_scene_transform PRIVATE ${TEST_WARNING_FLAGS})

# ── Camera frustum tests (pure logic, no GPU) ──────────────────────────────
add_executable(test_camera
    camera_safeguard_test.cpp
)
target_include_directories(test_camera PRIVATE
    ${CMAKE_SOURCE_DIR}/Vapor/include
    ${CMAKE_SOURCE_DIR}/Vapor/include/Vapor
)
target_link_libraries(test_camera PRIVATE
    Catch2::Catch2WithMain
    glm::glm
    fmt::fmt
)
target_compile_features(test_camera PRIVATE cxx_std_20)
target_compile_options(test_camera PRIVATE ${TEST_WARNING_FLAGS})

# ── Physics 3D tests (requires Jolt, linking Vapor) ──────────────────────
add_executable(test_physics
    physics_safeguard_test.cpp
)
target_link_libraries(test_physics PRIVATE
    Vapor
    Catch2::Catch2WithMain
    glm::glm
    fmt::fmt
)
target_compile_features(test_physics PRIVATE cxx_std_20)
target_compile_options(test_physics PRIVATE ${TEST_WARNING_FLAGS})

# ── Resource Manager tests (requires enkiTS, linking Vapor) ──────────────
add_executable(test_resource_manager
    resource_manager_safeguard_test.cpp
)
target_link_libraries(test_resource_manager PRIVATE
    Vapor
    Catch2::Catch2WithMain
    glm::glm
    fmt::fmt
)
target_compile_features(test_resource_manager PRIVATE cxx_std_20)
target_compile_options(test_resource_manager PRIVATE ${TEST_WARNING_FLAGS})

include(CTest)
include(Catch)
catch_discover_tests(test_action_system)
catch_discover_tests(test_scene_transform)
catch_discover_tests(test_camera)
catch_discover_tests(test_physics)
catch_discover_tests(test_resource_manager)
