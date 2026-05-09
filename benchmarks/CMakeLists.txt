find_package(Catch2 3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_executable(benchmark_example
    fibonacci_benchmark.cpp
)
target_link_libraries(benchmark_example PRIVATE
    Vapor
    Catch2::Catch2WithMain
    glm::glm
    fmt::fmt
)
target_compile_features(benchmark_example PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(benchmark_example)