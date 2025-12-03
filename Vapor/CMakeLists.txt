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
find_package(RmlUi CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)

add_subdirectory(metal)
add_subdirectory(MikkTSpace)

set(SOURCES
    src/action_manager.cpp
    src/audio_engine.cpp
    src/input_manager.cpp
    src/asset_manager.cpp
    src/asset_serializer.cpp
    src/camera.cpp
    src/graphics.cpp
    src/helper.cpp
    src/physics_3d.cpp
    src/character_controller.cpp
    src/vehicle_controller.cpp
    src/fluid_volume.cpp
    src/renderer_metal.cpp
    src/renderer_vulkan.cpp
    src/scene.cpp
    src/task_scheduler.cpp
    src/resource_manager.cpp
    src/jolt_enki_job_system.cpp
    src/engine_core.cpp
    src/rmlui_manager.cpp
    src/rmlui_renderer.cpp
    src/rmlui_system.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio
)

target_link_libraries(Vapor PUBLIC
    fmt::fmt
    glm::glm
    cereal::cereal
    SDL3::SDL3
    RmlUi::RmlUi
    EnTT::EnTT
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

find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)
file(GLOB_RECURSE SHADER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.comp"
)

foreach(GLSL ${SHADER_SOURCES})
    get_filename_component(SOURCE_NAME ${GLSL} NAME)
    set(SPIRV "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/${SOURCE_NAME}.spv")
    
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders"
        COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
        DEPENDS ${GLSL}
        COMMENT "Compiling ${SOURCE_NAME} to SPIR-V"
    )
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach()

add_custom_target(Vapor_compile_shaders ALL DEPENDS ${SPIRV_BINARY_FILES})
add_dependencies(Vapor Vapor_compile_shaders)

set(VAPOR_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" PARENT_SCOPE)


message(STATUS "Configuring Vapor package install...")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vapor-config-version.cmake"
    VERSION 0.0.1
    COMPATIBILITY SameMajorVersion
)

set(VAPOR_ASSETS_DIR "${CMAKE_INSTALL_DATADIR}/vapor/assets")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vapor-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/vapor-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_DATADIR}/vapor"
    PATH_VARS VAPOR_ASSETS_DIR
)

install(TARGETS Vapor metal mikktspace
    EXPORT VaporTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT VaporTargets
    FILE VaporTargets.cmake
    NAMESPACE Vapor::
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vapor"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vapor-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vapor-config-version.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/VaporMacros.cmake"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vapor"
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY imgui/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/imgui
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY assets/ DESTINATION "${CMAKE_INSTALL_DATADIR}/vapor/assets")

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vapor/assets"
)