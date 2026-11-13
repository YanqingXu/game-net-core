# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_kcp STATIC KcpEngine.cc KcpTransport.cc Upstream.cc)
add_library(GameNet::kcp ALIAS gamenet_kcp)
set_target_properties(gamenet_kcp PROPERTIES EXPORT_NAME kcp)
target_include_directories(gamenet_kcp PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_kcp PUBLIC cxx_std_23)
target_link_libraries(gamenet_kcp PUBLIC GameNet::udp)
gamenet_configure_sanitizers(gamenet_kcp)
if(MSVC)
    # The unchanged upstream declares extern-C entry points. Its private C++
    # allocator may throw, so /EHc's no-throw assumption must be disabled.
    # https://learn.microsoft.com/cpp/build/reference/eh-exception-handling-model
    target_compile_options(gamenet_kcp PRIVATE /EHsc-)
endif()
if(GAMENET_BUILD_TESTING)
    target_compile_definitions(gamenet_kcp PRIVATE GAMENET_INTERNAL_KCP_TEST_HOOKS=1)
endif()
install(TARGETS gamenet_kcp EXPORT GameNetKcpTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/kcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetKcpTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
install(FILES ${PROJECT_SOURCE_DIR}/third_party/kcp/LICENSE DESTINATION ${GAMENET_INSTALL_DOCDIR}/kcp)
