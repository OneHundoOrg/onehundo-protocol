cmake_minimum_required(VERSION 3.16)
project(onehundo_protocol VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ONEHUNDO_ENABLE_TESTS "Build C conformance tests" ON)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# Reference stub transport library (headless, portable C99)
add_library(onehundo_stub STATIC
    reference/c/transport_stub.c
)
target_include_directories(onehundo_stub
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/reference/c
)

# Reference POSIX transport library (Unix domain socket + POSIX shm_open)
if(UNIX)
    add_library(onehundo_posix STATIC
        reference/c/transport_posix.c
    )
    target_include_directories(onehundo_posix
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/c
    )
    if(NOT APPLE)
        target_link_libraries(onehundo_posix PUBLIC rt)
    endif()
endif()

# Reference Win32 transport library (Named Pipe + CreateFileMappingW)
if(WIN32)
    add_library(onehundo_win32 STATIC
        reference/c/transport_win32.c
    )
    target_include_directories(onehundo_win32
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/c
    )
endif()

# Reference bbport semantic integration example
add_executable(bbport_semantic_example
    reference/examples/bbport_semantic_example.c
)
target_link_libraries(bbport_semantic_example PRIVATE onehundo_stub)

if(ONEHUNDO_ENABLE_TESTS)
    enable_testing()

    find_package(Threads REQUIRED)

    add_executable(test_c_abi_and_stub
        conformance/tests/test_c_abi_and_stub.c
    )
    target_include_directories(test_c_abi_and_stub PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/reference/c
    )
    target_link_libraries(test_c_abi_and_stub PRIVATE onehundo_stub Threads::Threads)
    add_test(NAME c_abi_and_stub COMMAND test_c_abi_and_stub)

    if(UNIX)
        add_executable(test_posix_ipc
            conformance/tests/test_posix_ipc.c
        )
        target_include_directories(test_posix_ipc PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/c
        )
        target_link_libraries(test_posix_ipc PRIVATE onehundo_posix Threads::Threads)
        add_test(NAME posix_ipc COMMAND test_posix_ipc)
    endif()
endif()
