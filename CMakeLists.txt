cmake_minimum_required(VERSION 3.16)

project(
    runnerd
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 生成 compile_commands.json，供 clangd 获取项目的真实编译参数。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)




add_library(runnerd_protocol STATIC
    src/protocol.cpp
)

target_include_directories(runnerd_protocol PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(runnerd_protocol PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)




add_library(runnerd_job STATIC
    src/job.cpp
)

target_include_directories(runnerd_job PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(runnerd_job PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# encodeSubmitRequest() 调用了 validateJobSpec()，因此 runnerd_protocol 依赖 runnerd_job。
target_link_libraries(runnerd_protocol PUBLIC
    runnerd_job
)




add_executable(runnerd
    src/runnerd_main.cpp
    src/unix_socket.cpp
)

target_include_directories(runnerd PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runnerd PRIVATE
    runnerd_protocol
)

target_compile_options(runnerd PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# accept4 是 Linux/GNU 扩展接口。
target_compile_definitions(runnerd PRIVATE
    _GNU_SOURCE
)




add_executable(runnerctl
    src/runnerctl_main.cpp
    src/unix_socket.cpp
)

target_include_directories(runnerctl PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runnerctl PRIVATE
    runnerd_protocol
)

target_compile_options(runnerctl PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)




enable_testing()

add_executable(smoke_test
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

add_executable(protocol_test
    tests/protocol_test.cpp
)

target_link_libraries(protocol_test PRIVATE
    runnerd_protocol
)

target_compile_options(protocol_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(
    NAME protocol_test
    COMMAND protocol_test
)

add_executable(job_test
    tests/job_test.cpp
)

target_link_libraries(job_test PRIVATE
    runnerd_job
)

target_compile_options(job_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(
    NAME job_test
    COMMAND job_test
)

add_test(
    NAME runnerd_integration_test
    COMMAND bash
    ${PROJECT_SOURCE_DIR}/tests/runnerd_integration_test.sh
    $<TARGET_FILE:runnerd>
    $<TARGET_FILE:runnerctl>
)

# 防止服务端或客户端异常时让 CTest 一直等待。
set_tests_properties(runnerd_integration_test PROPERTIES
    TIMEOUT 10
)
