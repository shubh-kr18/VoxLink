cmake_minimum_required(VERSION 3.20)

project(AURA
    VERSION 0.1.0
    DESCRIPTION "C++ push-to-talk communication system"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aura_core
    core/src/floor_manager.cpp
)

target_include_directories(aura_core
    PUBLIC core/include
)

add_executable(aura_server
    apps/server/main.cpp
)

target_link_libraries(aura_server
    PRIVATE aura_core
)

enable_testing()

add_executable(floor_manager_test
    tests/unit/floor_manager_test.cpp
)

target_link_libraries(floor_manager_test
    PRIVATE aura_core
)

add_test(
    NAME floor_manager_test
    COMMAND floor_manager_test
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME tcp_server_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/integration/tcp_server_test.py
)

set_tests_properties(
    tcp_server_test
    PROPERTIES RUN_SERIAL TRUE
)
