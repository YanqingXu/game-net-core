# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(GameNetUpgradeConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED GAMENET_EXPECTED_VERSION)
    message(FATAL_ERROR "GAMENET_EXPECTED_VERSION is required")
endif()

find_package(GameNetCore ${GAMENET_EXPECTED_VERSION} EXACT REQUIRED)

if(GAMENET_EXPECTED_VERSION STREQUAL "0.3.0")
    if(NOT GameNetCore_LICENSE STREQUAL "Apache-2.0")
        message(FATAL_ERROR
            "GameNetCore 0.3.0 package license mismatch: '${GameNetCore_LICENSE}'"
        )
    endif()
endif()

add_executable(gamenet_upgrade_consumer main.cpp)
target_link_libraries(gamenet_upgrade_consumer PRIVATE GameNet::core)

enable_testing()
add_test(NAME gamenet.upgrade_consumer COMMAND gamenet_upgrade_consumer)
