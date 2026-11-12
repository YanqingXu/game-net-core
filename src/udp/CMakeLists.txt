# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_udp STATIC UdpSocket.cc)
add_library(GameNet::udp ALIAS gamenet_udp)
set_target_properties(gamenet_udp PROPERTIES EXPORT_NAME udp)
target_include_directories(gamenet_udp PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_udp PUBLIC cxx_std_23)
target_link_libraries(gamenet_udp PUBLIC GameNet::core)
gamenet_configure_sanitizers(gamenet_udp)
install(TARGETS gamenet_udp EXPORT GameNetUdpTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/udp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetUdpTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})

if(GAMENET_BUILD_TESTING)
    target_compile_definitions(gamenet_udp PRIVATE GAMENET_INTERNAL_UDP_TEST_HOOKS=1)
endif()
