# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(GameNetIoUringInstalledAcceptance LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "The installed acceptance sample requires Linux Release")
endif()

find_package(GameNetCore 0.3.1 EXACT CONFIG REQUIRED COMPONENTS experimental_io_uring)
foreach(component IN ITEMS core experimental_io_uring)
    get_target_property(configurations GameNet::${component} IMPORTED_CONFIGURATIONS)
    if(NOT "RELEASE" IN_LIST configurations)
        message(FATAL_ERROR "GameNet::${component} must be an installed Release target")
    endif()
endforeach()

add_executable(installed_io_uring_acceptance acceptance.cpp)
target_compile_features(installed_io_uring_acceptance PRIVATE cxx_std_23)
set_target_properties(installed_io_uring_acceptance PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(installed_io_uring_acceptance PRIVATE
    GameNet::core GameNet::experimental_io_uring)
option(GAMENET_COST_DIAGNOSTICS "Unscored allocation/copy call-site observer" OFF)
if(GAMENET_COST_DIAGNOSTICS)
    target_sources(installed_io_uring_acceptance PRIVATE ../cost_ledger/observer.cpp)
    target_compile_definitions(installed_io_uring_acceptance PRIVATE GAMENET_COST_DIAGNOSTICS)
    target_compile_options(installed_io_uring_acceptance PRIVATE -g -fno-pie
        -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memset)
    target_link_options(installed_io_uring_acceptance PRIVATE -no-pie
        -Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset)
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/installed-archives.txt"
    CONTENT "$<TARGET_FILE:GameNet::core>\n$<TARGET_FILE:GameNet::experimental_io_uring>\n")
