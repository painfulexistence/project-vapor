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
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost QUIET COMPONENTS pfr)

# Optional: FFmpeg for AV1 video recording/playback
option(VAPOR_ENABLE_FFMPEG "Enable FFmpeg for video recording and playback" ON)
if(VAPOR_ENABLE_FFMPEG)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG
            libavcodec
            libavformat
            libavutil
            libswscale
            libswresample
        )
    endif()
endif()
if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found — video recording/playback enabled")
else()
    message(STATUS "FFmpeg not found — video recording/playback disabled")
endif()

if(APPLE)
    add_subdirectory(metal)
endif()
add_subdirectory(MikkTSpace)

set(SOURCES
    src/action_manager.cpp
    src/audio_engine.cpp
    src/file_system.cpp
    src/input_manager.cpp
    src/asset_manager.cpp
    src/scene_blueprint.cpp
    src/asset_serializer.cpp
    src/camera.cpp
    src/debug_draw.cpp
    src/atlas_baker.cpp
    src/font_manager.cpp
    src/graphics.cpp
    src/helper.cpp
    src/stats_log.cpp
    src/physics_3d.cpp
    src/physics_debug_renderer.cpp
    src/character_controller.cpp
    src/vehicle_controller.cpp
    src/fluid_volume.cpp
    # RHI architecture files (replacing renderer_metal.cpp and renderer_vulkan.cpp)
    src/renderer.cpp
    src/rhi_vulkan.cpp
    src/rml_renderer_rhi.cpp
    src/render_scene.cpp
    src/task_scheduler.cpp
    src/resource_manager.cpp
    src/jolt_enki_job_system.cpp
    src/engine_core.cpp
    src/rmlui_manager.cpp
    src/rmlui_system.cpp
    src/video_recorder.cpp
    src/video_player.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)

if(APPLE)
    list(APPEND SOURCES
        # Native full-feature Metal renderer (45 passes, RT/GIBS/water/clouds/…).
        # createRenderer() routes the Metal backend here; Vulkan uses the RHI
        # renderer (src/renderer.cpp) on every platform.
        src/renderer_metal.cpp
        src/gibs_manager.cpp
        src/rhi_metal.cpp
        src/rml_renderer_metal.cpp
        src/ui_renderer_metal.cpp
        imgui/backends/imgui_impl_metal.mm
    )
else()
    list(APPEND SOURCES
        src/ui_renderer_stub.cpp
    )
endif()

add_library(Vapor ${SOURCES})
set_target_properties(Vapor PROPERTIES POSITION_INDEPENDENT_CODE ON)
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

if(FFMPEG_FOUND)
    target_compile_definitions(Vapor PUBLIC VAPOR_HAS_FFMPEG)
    target_include_directories(Vapor PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(Vapor PUBLIC ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(Vapor PRIVATE ${FFMPEG_LIBRARIES})
endif()

target_link_libraries(Vapor PUBLIC
    fmt::fmt
    glm::glm
    cereal::cereal
    SDL3::SDL3
    RmlUi::RmlUi
    EnTT::EnTT
    nlohmann_json::nlohmann_json
)
if(Boost_FOUND)
    message(STATUS "Boost.PFR found — component auto-reflection enabled")
    target_link_libraries(Vapor PUBLIC Boost::pfr)
    target_compile_definitions(Vapor PUBLIC VAPOR_HAS_BOOST_PFR)
else()
    message(STATUS "Boost.PFR not found — component auto-reflection disabled")
endif()
target_link_libraries(Vapor PRIVATE
    Vulkan::Vulkan
    taywee::args
    mikktspace
    Jolt::Jolt
    enkiTS::enkiTS
    Tracy::TracyClient
)

if(APPLE)
    target_link_libraries(Vapor PRIVATE metal)
endif()

include(AssetPipeline)
vapor_compile_glsl_shaders(Vapor "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")

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

if(APPLE)
    set(PLATFORM_LIBS metal)
endif()
install(TARGETS Vapor ${PLATFORM_LIBS} mikktspace
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/helpers/AssetPipeline.cmake"
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

add_subdirectory(capi)
