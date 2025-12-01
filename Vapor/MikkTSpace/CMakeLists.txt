enable_language(C)
add_library(mikktspace STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/mikktspace.c"
)
target_include_directories(mikktspace PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

install(FILES mikktspace.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})