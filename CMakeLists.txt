cmake_minimum_required(VERSION 3.20.0)

project(vapor LANGUAGES CXX)

set(FETCHCONTENT_QUIET FALSE)
include(FetchContent)

add_subdirectory(Vapor)
add_subdirectory(Vaporware)