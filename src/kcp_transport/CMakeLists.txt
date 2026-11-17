# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_kcp_transport STATIC KcpTransportEndpoint.cc)
add_library(GameNet::kcp_transport ALIAS gamenet_kcp_transport)
set_target_properties(gamenet_kcp_transport PROPERTIES EXPORT_NAME kcp_transport)
target_include_directories(gamenet_kcp_transport PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_kcp_transport PUBLIC cxx_std_23)
target_compile_options(gamenet_kcp_transport PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_kcp_transport PUBLIC GameNet::kcp GameNet::transport)
gamenet_configure_sanitizers(gamenet_kcp_transport)
install(TARGETS gamenet_kcp_transport EXPORT GameNetKcpTransportTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/kcp_transport
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetKcpTransportTargets NAMESPACE GameNet::
    DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
