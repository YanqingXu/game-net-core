# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_rpc_tls STATIC RpcTlsSession.cc)
add_library(GameNet::rpc_tls ALIAS gamenet_rpc_tls)
set_target_properties(gamenet_rpc_tls PROPERTIES EXPORT_NAME rpc_tls)
target_include_directories(gamenet_rpc_tls PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_rpc_tls PUBLIC cxx_std_23)
target_compile_options(gamenet_rpc_tls PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_rpc_tls PUBLIC GameNet::rpc GameNet::tls)
gamenet_configure_sanitizers(gamenet_rpc_tls)
install(TARGETS gamenet_rpc_tls EXPORT GameNetRpcTlsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/rpc_tls
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetRpcTlsTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
