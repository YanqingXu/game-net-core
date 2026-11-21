# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(GameNetKcpGatewayReplay LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Configure the gateway replay with -DCMAKE_BUILD_TYPE=Release")
endif()

find_package(GameNetCore CONFIG REQUIRED COMPONENTS kcp_transport)
foreach(component IN ITEMS core udp kcp kcp_transport game_session broadcast)
    get_target_property(configs GameNet::${component} IMPORTED_CONFIGURATIONS)
    if(NOT "RELEASE" IN_LIST configs)
        message(FATAL_ERROR "Gateway replay requires a Release GameNet package (${component})")
    endif()
endforeach()

set(profile_file "${CMAKE_CURRENT_SOURCE_DIR}/gateway-q3-q5.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${profile_file}")
file(READ "${profile_file}" profile)
string(REPLACE "\r\n" "\n" profile "${profile}")
string(SHA256 profile_sha256 "${profile}")
foreach(field IN ITEMS epoch_us broadcast_interval_us first_broadcast_us warmup_broadcasts
                        broadcast_recipients slow_recipients router_fanout_limit
                        dispatch_task_bytes oversize_body_bytes oversize_at_us
                        fault_begin_us pressure_at_us pressure_messages pressure_body_bytes
                        fault_restore_us smoke_epochs measure_epochs)
    string(JSON ${field} GET "${profile}" workload "${field}")
    if(NOT "${${field}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Invalid gateway profile number: ${field}")
    endif()
endforeach()
foreach(field IN ITEMS warmup_body broadcast_body recovery_body)
    string(JSON ${field} GET "${profile}" workload "${field}")
    if(NOT "${${field}}" MATCHES "^[A-Za-z0-9:._-]+$")
        message(FATAL_ERROR "Invalid gateway profile body: ${field}")
    endif()
endforeach()
configure_file(ReplayProfile.h.in ReplayProfile.h @ONLY)

add_executable(gamenet_kcp_gateway_replay replay.cpp)
target_compile_features(gamenet_kcp_gateway_replay PRIVATE cxx_std_23)
set_target_properties(gamenet_kcp_gateway_replay PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(gamenet_kcp_gateway_replay PRIVATE
    "$<$<CONFIG:Release>:GAMENET_GATEWAY_REPLAY_RELEASE>")
target_include_directories(gamenet_kcp_gateway_replay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gamenet_kcp_gateway_replay PRIVATE
    GameNet::kcp_transport GameNet::game_session GameNet::broadcast)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gateway-replay-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:gamenet_kcp_gateway_replay>\n${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n")
