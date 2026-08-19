add_library(gamenet_experimental_io_uring STATIC
    IoUringCompletionEngine.cc
    IoUringEventLoopPump.cc
)
add_library(GameNet::experimental ALIAS gamenet_experimental_io_uring)

target_include_directories(gamenet_experimental_io_uring
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_compile_features(gamenet_experimental_io_uring PUBLIC cxx_std_23)
target_compile_options(gamenet_experimental_io_uring
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(gamenet_experimental_io_uring PUBLIC GameNet::core)
gamenet_configure_sanitizers(gamenet_experimental_io_uring)
