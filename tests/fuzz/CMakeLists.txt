if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "GAMENET_BUILD_FUZZING requires Clang/libFuzzer")
endif()

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
