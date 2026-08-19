add_executable(gamenet_core_benchmark
    core/main.cpp
)

add_executable(gamenet_phase4_benchmark
    phase4/main.cpp
)

add_executable(gamenet_capacity_profile
    capacity/main.cpp
)

add_executable(gamenet_multi_io_queued_benchmark
    runtime_profiles/multi_io_queued.cpp
)

add_executable(gamenet_multi_io_fixed_tick_benchmark
    runtime_profiles/multi_io_fixed_tick.cpp
)

add_executable(gamenet_multi_io_sharded_hybrid_benchmark
    runtime_profiles/multi_io_sharded_hybrid.cpp
)

if(GAMENET_ENABLE_EXPERIMENTAL)
    add_executable(gamenet_io_uring_one_shot_benchmark
        io_uring/one_shot.cpp
    )
    add_executable(gamenet_io_uring_shared_tcp_hub_benchmark
        io_uring/shared_tcp_hub.cpp
    )
    target_link_libraries(gamenet_io_uring_one_shot_benchmark PRIVATE
        GameNet::experimental
    )
    target_compile_definitions(gamenet_io_uring_one_shot_benchmark PRIVATE
        GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    )
    target_link_libraries(gamenet_io_uring_shared_tcp_hub_benchmark PRIVATE
        GameNet::experimental
    )
    target_compile_definitions(
        gamenet_io_uring_shared_tcp_hub_benchmark PRIVATE
        GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    )
endif()

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
target_link_libraries(gamenet_multi_io_queued_benchmark PRIVATE
    gamenet_runtime_profile_demo_support
)
target_link_libraries(gamenet_multi_io_fixed_tick_benchmark PRIVATE
    gamenet_runtime_profile_demo_support
)
target_link_libraries(gamenet_multi_io_sharded_hybrid_benchmark PRIVATE
    gamenet_runtime_profile_demo_support
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
target_compile_definitions(gamenet_multi_io_queued_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
target_compile_definitions(gamenet_multi_io_fixed_tick_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
target_compile_definitions(gamenet_multi_io_sharded_hybrid_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)

if(WIN32)
    target_link_libraries(gamenet_core_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_phase4_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_capacity_profile PRIVATE psapi)
    target_link_libraries(gamenet_multi_io_queued_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_multi_io_fixed_tick_benchmark PRIVATE psapi)
    target_link_libraries(gamenet_multi_io_sharded_hybrid_benchmark PRIVATE psapi)
endif()
