cmake_minimum_required(VERSION 3.16)

project(runnerd
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(runnerd
    src/runnerd_main.cpp
)

add_executable(runnerctl
    src/runnerctl_main.cpp
)

target_compile_options( runnerd PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options( runnerctl PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()

add_executable( smoke_test
    tests/smoke_test.cpp
)

target_compile_options(smoke_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(
    NAME smoke_test
    COMMAND smoke_test
)
