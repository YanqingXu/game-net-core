# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "GAMENET_BUILD_FUZZING requires Clang/libFuzzer")
endif()

if(TARGET gamenet_websocket)
    target_compile_options(gamenet_websocket PRIVATE -fsanitize=fuzzer-no-link)
    add_executable(gamenet_fuzz_websocket websocket/fuzz_websocket.cpp)
    target_include_directories(gamenet_fuzz_websocket PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(gamenet_fuzz_websocket PRIVATE GameNet::websocket)
    target_compile_options(gamenet_fuzz_websocket PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(gamenet_fuzz_websocket PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(gamenet_fuzz_websocket_session websocket/fuzz_websocket_session.cpp)
    target_include_directories(gamenet_fuzz_websocket_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(gamenet_fuzz_websocket_session PRIVATE GameNet::websocket)
    target_compile_options(gamenet_fuzz_websocket_session PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(gamenet_fuzz_websocket_session PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Instrument production code too: entry-point-only coverage misses RPC branches.
# Fuzz builds are opt-in development builds, never package evidence.
foreach(component gamenet_core gamenet_protocol gamenet_rpc)
    target_compile_options(${component} PRIVATE -fsanitize=fuzzer-no-link)
endforeach()
add_executable(gamenet_fuzz_rpc_codec rpc/fuzz_rpc_codec.cpp)
add_executable(gamenet_fuzz_rpc_channel rpc/fuzz_rpc_channel.cpp)
foreach(kind codec channel)
    target_include_directories(gamenet_fuzz_rpc_${kind} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(gamenet_fuzz_rpc_${kind} PRIVATE GameNet::rpc)
    target_compile_features(gamenet_fuzz_rpc_${kind} PRIVATE cxx_std_23)
    target_compile_options(gamenet_fuzz_rpc_${kind} PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(gamenet_fuzz_rpc_${kind} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()

add_executable(gamenet_fuzz_packet_framer
    protocol/fuzz_packet_framer.cpp
)
target_link_libraries(gamenet_fuzz_packet_framer PRIVATE GameNet::protocol)
target_compile_features(gamenet_fuzz_packet_framer PRIVATE cxx_std_23)
target_compile_options(gamenet_fuzz_packet_framer PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)
target_link_options(gamenet_fuzz_packet_framer PRIVATE
    -fsanitize=fuzzer,address,undefined
)

if(TARGET gamenet_hp1_packet_framer_view)
    add_executable(gamenet_fuzz_packet_framer_view
        prestudy/fuzz_packet_framer_view.cpp
    )
    target_link_libraries(gamenet_fuzz_packet_framer_view PRIVATE
        gamenet_hp1_packet_framer_view GameNet::core
    )
    target_compile_features(gamenet_fuzz_packet_framer_view PRIVATE cxx_std_23)
    target_compile_options(gamenet_fuzz_packet_framer_view PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(gamenet_fuzz_packet_framer_view PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
endif()
