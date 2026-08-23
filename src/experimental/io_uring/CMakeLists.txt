# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

add_library(gamenet_experimental_io_uring STATIC
    IoUringCompletionEngine.cc
    IoUringEventLoopPump.cc
    IoUringTcpConnectionDriver.cc
    IoUringTcpConnectionAdapter.cc
    IoUringTcpConnectionHub.cc
    IoUringTcpServer.cc
    IoUringTcpMultiOwnerServer.cc
    IoUringTcpClient.cc
)
add_library(GameNet::experimental_io_uring ALIAS gamenet_experimental_io_uring)

set_target_properties(gamenet_experimental_io_uring PROPERTIES
    EXPORT_NAME experimental_io_uring
)

target_include_directories(gamenet_experimental_io_uring
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(gamenet_experimental_io_uring
    PUBLIC GAMENET_EXPERIMENTAL_IO_URING=1
)
target_compile_features(gamenet_experimental_io_uring PUBLIC cxx_std_23)
target_compile_options(gamenet_experimental_io_uring
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(gamenet_experimental_io_uring PUBLIC GameNet::core)
gamenet_configure_sanitizers(gamenet_experimental_io_uring)

install(TARGETS gamenet_experimental_io_uring
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/experimental/io_uring
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet/experimental
)
