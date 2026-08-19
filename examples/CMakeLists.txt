add_executable(gamenet_echo_server
    echo_server/main.cpp
)

target_link_libraries(gamenet_echo_server PRIVATE GameNet::core)
set_target_properties(gamenet_echo_server PROPERTIES OUTPUT_NAME echo_server)

add_executable(gamenet_echo_client
    echo_client/main.cpp
)

target_link_libraries(gamenet_echo_client PRIVATE GameNet::core)
set_target_properties(gamenet_echo_client PROPERTIES OUTPUT_NAME echo_client)

add_library(gamenet_pipeline_demo_support STATIC
    game_server_pipeline_demo/GameServerPipeline.cc
)
target_include_directories(gamenet_pipeline_demo_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gamenet_pipeline_demo_support
    PUBLIC
        GameNet::core
        GameNet::protocol
        GameNet::transport
        GameNet::game_session
        GameNet::game_logic
)
target_compile_features(gamenet_pipeline_demo_support PUBLIC cxx_std_23)
target_compile_options(gamenet_pipeline_demo_support PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
gamenet_configure_sanitizers(gamenet_pipeline_demo_support)

add_executable(gamenet_game_server_pipeline_demo
    game_server_pipeline_demo/main.cpp
)
target_link_libraries(gamenet_game_server_pipeline_demo PRIVATE gamenet_pipeline_demo_support)
set_target_properties(gamenet_game_server_pipeline_demo PROPERTIES OUTPUT_NAME game_server_pipeline_demo)

add_library(gamenet_runtime_profile_demo_support STATIC
    runtime_profiles/MultiIoQueuedEvent.cc
    runtime_profiles/SingleLoopInlineEvent.cc
)
target_include_directories(
    gamenet_runtime_profile_demo_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
    gamenet_runtime_profile_demo_support
    PUBLIC GameNet::core GameNet::protocol GameNet::transport GameNet::game_logic
)
target_compile_features(gamenet_runtime_profile_demo_support PUBLIC cxx_std_23)
target_compile_options(
    gamenet_runtime_profile_demo_support
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
gamenet_configure_sanitizers(gamenet_runtime_profile_demo_support)

add_executable(gamenet_single_loop_inline_echo
    runtime_profiles/single_loop_inline_echo.cpp
)
target_link_libraries(
    gamenet_single_loop_inline_echo
    PRIVATE gamenet_runtime_profile_demo_support
)
set_target_properties(
    gamenet_single_loop_inline_echo
    PROPERTIES OUTPUT_NAME single_loop_inline_echo
)

add_executable(gamenet_multi_io_queued_echo
    runtime_profiles/multi_io_queued_echo.cpp
)
target_link_libraries(
    gamenet_multi_io_queued_echo
    PRIVATE gamenet_runtime_profile_demo_support
)
set_target_properties(
    gamenet_multi_io_queued_echo
    PROPERTIES OUTPUT_NAME multi_io_queued_echo
)
