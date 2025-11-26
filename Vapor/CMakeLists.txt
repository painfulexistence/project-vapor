# ===================================================================
#           project-vapor/Vapor/CMakeLists.txt
# ===================================================================

find_package(args CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
find_package(cereal CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(metal)
add_subdirectory(MikkTSpace)

# 1. 定義 "Vapor" 函式庫
add_library(Vapor "") # 來源檔案稍後加入

target_sources(Vapor PRIVATE
    src/asset_manager.cpp
    src/asset_serializer.cpp
    src/camera.cpp
    src/graphics.cpp
    src/helper.cpp
    src/physics_3d.cpp
    src/renderer_metal.cpp
    src/renderer_vulkan.cpp
    src/scene.cpp
    # IMGUI 來源檔案
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
    imgui/backends/imgui_impl_metal.mm
)

target_compile_features(Vapor PRIVATE cxx_std_20)
set_target_properties(Vapor PROPERTIES CXX_EXTENSIONS OFF)

# 2. 設定公開和私有的 Include 路徑
target_include_directories(Vapor PUBLIC
    # 當安裝後，告訴使用者 include 目錄在 <prefix>/include
    $<INSTALL_INTERFACE:include>
    # 在本地建置時，告訴使用者 include 目錄在目前的 "include" 資料夾
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(Vapor PRIVATE
    ${TINYGLTF_INCLUDE_DIRS}
    imgui
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Vapor"
    "${CMAKE_CURRENT_SOURCE_DIR}"
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
)


# 4. 安裝函式庫 (Skip EXPORT as per request)
install(TARGETS Vapor 
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 5. 安裝公開標頭檔
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})