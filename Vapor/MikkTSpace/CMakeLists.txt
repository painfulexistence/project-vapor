enable_language(C)
add_library(mikktspace STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/mikktspace.c"
)
target_include_directories(mikktspace PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)