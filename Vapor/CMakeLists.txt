find_package(args CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
find_package(cereal CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)

add_subdirectory(metal)
add_subdirectory(MikkTSpace)

set(SOURCES
    src/asset_manager.cpp
    src/asset_serializer.cpp
    src/camera.cpp
    src/graphics.cpp
    src/helper.cpp
    src/physics_3d.cpp
    src/renderer_metal.cpp
    src/renderer_vulkan.cpp
    src/scene.cpp
    src/task_scheduler.cpp
    src/resource_manager.cpp
    src/jolt_enki_job_system.cpp
    src/engine_core.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
    imgui/backends/imgui_impl_metal.mm
)

add_library(Vapor ${SOURCES})
target_compile_features(Vapor PRIVATE cxx_std_20)
set_target_properties(Vapor PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(Vapor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/imgui>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/imgui>
)
target_include_directories(Vapor PRIVATE
    ${TINYGLTF_INCLUDE_DIRS}
    imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Vapor
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Vapor PUBLIC
    fmt::fmt
    glm::glm
    cereal::cereal
    SDL3::SDL3
)
target_link_libraries(Vapor PRIVATE
    metal
    Vulkan::Vulkan
    taywee::args
    mikktspace
    Jolt::Jolt
    enkiTS::enkiTS
    Tracy::TracyClient
)


install(TARGETS Vapor
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY imgui/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/imgui
    FILES_MATCHING PATTERN "*.h"
)