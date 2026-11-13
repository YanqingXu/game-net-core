# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_udp_transport STATIC UdpTransportEndpoint.cc)
add_library(GameNet::udp_transport ALIAS gamenet_udp_transport)
set_target_properties(gamenet_udp_transport PROPERTIES EXPORT_NAME udp_transport)
target_include_directories(gamenet_udp_transport PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_udp_transport PUBLIC cxx_std_23)
target_compile_options(gamenet_udp_transport PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_udp_transport PUBLIC GameNet::udp GameNet::transport)
gamenet_configure_sanitizers(gamenet_udp_transport)
install(TARGETS gamenet_udp_transport EXPORT GameNetUdpTransportTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/udp_transport DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetUdpTransportTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
