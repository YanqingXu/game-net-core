add_executable(gamenet_core_benchmark
    core/main.cpp
)

add_executable(gamenet_phase4_benchmark
    phase4/main.cpp
)

add_executable(gamenet_capacity_profile
    capacity/main.cpp
)

target_link_libraries(gamenet_core_benchmark PRIVATE GameNet::core)
target_link_libraries(gamenet_phase4_benchmark PRIVATE
    GameNet::protocol
    GameNet::game_session
    GameNet::game_logic
    GameNet::broadcast
)
target_link_libraries(gamenet_capacity_profile PRIVATE
    GameNet::core
    GameNet::transport
    GameNet::broadcast
)
target_compile_definitions(gamenet_core_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
target_compile_definitions(gamenet_phase4_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
target_compile_definitions(gamenet_capacity_profile PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)

if(WIN32)
    target_link_libraries(gamenet_core_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_phase4_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_capacity_profile PRIVATE psapi)
endif()
