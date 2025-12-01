set(GAMEPLAY_SOURCES
    gameplay_module.cpp
)

add_library(Gameplay SHARED ${GAMEPLAY_SOURCES})

target_include_directories(Gameplay PRIVATE
    ${CMAKE_SOURCE_DIR}/Vapor/include
    ${CMAKE_SOURCE_DIR}/Vaporware/src
)

target_link_libraries(Gameplay PRIVATE
    Vapor
)

# Set symbol visibility for non-Windows platforms
if(NOT WIN32)
    target_compile_options(Gameplay PRIVATE -fvisibility=hidden)
endif()

# Output to predictable location
set_target_properties(Gameplay PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gameplay"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gameplay"
    # Disable RPATH on Linux/macOS for easier testing
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH TRUE
)

# For faster iteration during development
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(Gameplay PRIVATE -O0 -g)
endif()
