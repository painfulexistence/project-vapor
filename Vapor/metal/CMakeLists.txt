add_library(metal INTERFACE)
target_include_directories(metal INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/metal-cpp"
)
target_link_libraries(metal INTERFACE
    "-framework Metal"
    "-framework Foundation"
    "-framework QuartzCore"
)