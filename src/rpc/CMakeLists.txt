# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_rpc STATIC RpcCodec.cc RpcChannel.cc)
add_library(GameNet::rpc ALIAS gamenet_rpc)
set_target_properties(gamenet_rpc PROPERTIES EXPORT_NAME rpc)
target_include_directories(gamenet_rpc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(gamenet_rpc PUBLIC GameNet::core GameNet::protocol)
target_compile_features(gamenet_rpc PUBLIC cxx_std_23)
target_compile_options(gamenet_rpc PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
gamenet_configure_sanitizers(gamenet_rpc)
install(TARGETS gamenet_rpc EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
