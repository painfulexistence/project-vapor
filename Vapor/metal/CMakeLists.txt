add_library(metal INTERFACE)
target_include_directories(metal INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/metal-cpp>
    $<INSTALL_INTERFACE:include/metal-cpp>
)
target_link_libraries(metal INTERFACE
    "-framework Metal"
    "-framework Foundation"
    "-framework QuartzCore"
)

install(DIRECTORY metal-cpp/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/metal-cpp)