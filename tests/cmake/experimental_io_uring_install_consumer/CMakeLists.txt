# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(GameNetExperimentalIoUringInstallConsumer LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "experimental_io_uring consumer requires Linux")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(
    GameNetCore 0.3.0 EXACT REQUIRED
    COMPONENTS experimental_io_uring
)

if(NOT TARGET GameNet::experimental_io_uring)
    message(FATAL_ERROR "GameNet::experimental_io_uring target is missing")
endif()
if(NOT GameNetCore_EXPERIMENTAL_IO_URING_API_VERSION STREQUAL "0.1.0")
    message(FATAL_ERROR
        "unexpected experimental io_uring API version: "
        "'${GameNetCore_EXPERIMENTAL_IO_URING_API_VERSION}'"
    )
endif()

add_executable(gamenet_experimental_io_uring_install_consumer main.cpp)
target_link_libraries(gamenet_experimental_io_uring_install_consumer
    PRIVATE GameNet::experimental_io_uring
)

enable_testing()
add_test(
    NAME gamenet.experimental_io_uring_install_consumer
    COMMAND gamenet_experimental_io_uring_install_consumer
)
