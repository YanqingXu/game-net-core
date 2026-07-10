add_executable(gamenet_core_benchmark
    core/main.cpp
)

target_link_libraries(gamenet_core_benchmark PRIVATE GameNet::core)
target_compile_definitions(gamenet_core_benchmark PRIVATE
    GAMENET_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)

if(WIN32)
    target_link_libraries(gamenet_core_benchmark PRIVATE psapi)
endif()
