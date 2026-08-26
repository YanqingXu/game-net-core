# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

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

# HP1 EXPERIMENTAL-PRESTUDY: isolated borrowed-view framing candidate. These
# targets exist only when GAMENET_BUILD_BENCHMARKS=ON and are never installed.
add_library(gamenet_hp1_packet_framer_view STATIC
    prestudy/hp1/PacketFramerView.cc
)
target_include_directories(gamenet_hp1_packet_framer_view
    PUBLIC ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_link_libraries(gamenet_hp1_packet_framer_view PUBLIC GameNet::protocol)
target_compile_features(gamenet_hp1_packet_framer_view PUBLIC cxx_std_23)
target_compile_options(gamenet_hp1_packet_framer_view
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_hp1_packet_framer_view)

add_executable(gamenet_hp1_packet_framer_view_benchmark
    prestudy/hp1/main.cpp
)
target_link_libraries(gamenet_hp1_packet_framer_view_benchmark PRIVATE
    gamenet_hp1_packet_framer_view
)
target_compile_definitions(gamenet_hp1_packet_framer_view_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp1_packet_framer_view_benchmark)

add_custom_target(gamenet_hp1_packet_framer_view_prestudy
    DEPENDS gamenet_hp1_packet_framer_view_benchmark
)

# HP2 EXPERIMENTAL-PRESTUDY: fixed-capacity SPSC mailbox/source/outbox. It is
# header-only inside the benchmark tree, default-off, non-installed, and does
# not alter the production EventLoop or Profile B implementation.
add_library(gamenet_hp2_spsc_mailbox INTERFACE)
target_include_directories(gamenet_hp2_spsc_mailbox
    INTERFACE ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_link_libraries(gamenet_hp2_spsc_mailbox
    INTERFACE gamenet_hp1_packet_framer_view
)
target_compile_features(gamenet_hp2_spsc_mailbox INTERFACE cxx_std_23)

add_executable(gamenet_hp2_spsc_mailbox_benchmark
    prestudy/hp2/main.cpp
)
target_link_libraries(gamenet_hp2_spsc_mailbox_benchmark PRIVATE
    gamenet_hp2_spsc_mailbox
)
target_compile_definitions(gamenet_hp2_spsc_mailbox_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp2_spsc_mailbox_benchmark)

add_custom_target(gamenet_hp2_spsc_mailbox_prestudy
    DEPENDS gamenet_hp2_spsc_mailbox_benchmark
)

# HP3 EXPERIMENTAL-PRESTUDY: portable slot/generation decode mechanics only.
# It owns no epoll descriptor and never replaces the production Linux port.
add_library(gamenet_hp3_epoll_slot_dispatch STATIC
    prestudy/hp3/EpollSlotDispatch.cc
)
target_include_directories(gamenet_hp3_epoll_slot_dispatch
    PUBLIC ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_compile_features(gamenet_hp3_epoll_slot_dispatch PUBLIC cxx_std_23)
target_compile_options(gamenet_hp3_epoll_slot_dispatch
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_hp3_epoll_slot_dispatch)

add_executable(gamenet_hp3_epoll_slot_dispatch_benchmark
    prestudy/hp3/main.cpp
)
target_link_libraries(gamenet_hp3_epoll_slot_dispatch_benchmark PRIVATE
    gamenet_hp3_epoll_slot_dispatch
)
target_compile_definitions(gamenet_hp3_epoll_slot_dispatch_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp3_epoll_slot_dispatch_benchmark)

add_custom_target(gamenet_hp3_epoll_slot_dispatch_prestudy
    DEPENDS gamenet_hp3_epoll_slot_dispatch_benchmark
)

# HP4 EXPERIMENTAL-PRESTUDY: fixed owned/shared segment mechanics and owner
# broadcast batching. It invokes no socket API and is never installed.
add_library(gamenet_hp4_output_segment_chain STATIC
    prestudy/hp4/OutputSegmentChain.cc
)
target_include_directories(gamenet_hp4_output_segment_chain
    PUBLIC ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_compile_features(gamenet_hp4_output_segment_chain PUBLIC cxx_std_23)
target_compile_options(gamenet_hp4_output_segment_chain
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_hp4_output_segment_chain)

add_executable(gamenet_hp4_output_segment_chain_benchmark
    prestudy/hp4/main.cpp
)
target_link_libraries(gamenet_hp4_output_segment_chain_benchmark PRIVATE
    gamenet_hp4_output_segment_chain
)
target_compile_definitions(gamenet_hp4_output_segment_chain_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp4_output_segment_chain_benchmark)

add_custom_target(gamenet_hp4_output_segment_chain_prestudy
    DEPENDS gamenet_hp4_output_segment_chain_benchmark
)

# HP5 EXPERIMENTAL-PRESTUDY: fixed owner-local connection/loop accounting with
# conservative shared server/global credit chunks. It is never installed.
add_library(gamenet_hp5_credit_lease STATIC
    prestudy/hp5/CreditLease.cc
)
target_include_directories(gamenet_hp5_credit_lease
    PUBLIC ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_compile_features(gamenet_hp5_credit_lease PUBLIC cxx_std_23)
target_compile_options(gamenet_hp5_credit_lease
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_hp5_credit_lease)

add_executable(gamenet_hp5_credit_lease_benchmark
    prestudy/hp5/main.cpp
)
target_link_libraries(gamenet_hp5_credit_lease_benchmark PRIVATE
    gamenet_hp5_credit_lease
)
target_compile_definitions(gamenet_hp5_credit_lease_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp5_credit_lease_benchmark)

add_custom_target(gamenet_hp5_credit_lease_prestudy
    DEPENDS gamenet_hp5_credit_lease_benchmark
)

# HP6-A EXPERIMENTAL-PRESTUDY: pure fixed-storage scheduling quota model.
# HP6-B pools and HP6-C layout remain skipped by evidence.
add_library(gamenet_hp6_adaptive_scheduler STATIC
    prestudy/hp6/AdaptivePhaseScheduler.cc
)
target_include_directories(gamenet_hp6_adaptive_scheduler
    PUBLIC ${PROJECT_SOURCE_DIR}/benchmarks/prestudy
)
target_compile_features(gamenet_hp6_adaptive_scheduler PUBLIC cxx_std_23)
target_compile_options(gamenet_hp6_adaptive_scheduler
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_hp6_adaptive_scheduler)

add_executable(gamenet_hp6_adaptive_scheduler_benchmark
    prestudy/hp6/main.cpp
)
target_link_libraries(gamenet_hp6_adaptive_scheduler_benchmark PRIVATE
    gamenet_hp6_adaptive_scheduler
)
target_compile_definitions(gamenet_hp6_adaptive_scheduler_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
gamenet_configure_sanitizers(gamenet_hp6_adaptive_scheduler_benchmark)

add_custom_target(gamenet_hp6_adaptive_scheduler_prestudy
    DEPENDS gamenet_hp6_adaptive_scheduler_benchmark
)

# HP0 is a suite target: tools/run_hot_path_cost.py drives these existing,
# semantically validated executables and freezes their raw JSON in one cost
# ledger.  It remains behind GAMENET_BUILD_BENCHMARKS, is not installed, and
# is intentionally not registered with CTest.
add_custom_target(gamenet_hot_path_benchmark
    DEPENDS
        gamenet_core_benchmark
        gamenet_phase4_benchmark
        gamenet_capacity_profile
        gamenet_multi_io_queued_benchmark
)

if(GAMENET_ENABLE_EXPERIMENTAL)
    add_executable(gamenet_io_uring_one_shot_benchmark
        io_uring/one_shot.cpp
    )
    add_executable(gamenet_io_uring_shared_tcp_hub_benchmark
        io_uring/shared_tcp_hub.cpp
    )
    add_executable(gamenet_ioe_x10_listener_comparison_benchmark
        io_uring/listener_comparison.cpp
    )
    target_link_libraries(gamenet_io_uring_one_shot_benchmark PRIVATE
        GameNet::experimental_io_uring
    )
    target_compile_definitions(gamenet_io_uring_one_shot_benchmark PRIVATE
        GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    )
    target_link_libraries(gamenet_io_uring_shared_tcp_hub_benchmark PRIVATE
        GameNet::experimental_io_uring
    )
    target_compile_definitions(
        gamenet_io_uring_shared_tcp_hub_benchmark PRIVATE
        GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
    )
    target_link_libraries(gamenet_ioe_x10_listener_comparison_benchmark PRIVATE
        GameNet::experimental_io_uring
    )
    target_compile_definitions(
        gamenet_ioe_x10_listener_comparison_benchmark PRIVATE
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
