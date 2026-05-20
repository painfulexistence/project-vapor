project(VaporCAPI VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

add_library(VaporCAPI SHARED
    src/capi.cpp
)

target_compile_features(VaporCAPI PRIVATE cxx_std_20)
set_target_properties(VaporCAPI PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(VaporCAPI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(VaporCAPI PRIVATE Vapor)

if(MSVC)
    target_compile_options(VaporCAPI PRIVATE /W4 /wd4100)
else()
    target_compile_options(VaporCAPI PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

install(TARGETS VaporCAPI
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/VirgaNativeAPI.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
