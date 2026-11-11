# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL)
add_library(gamenet_tls STATIC TlsContext.cc TlsStream.cc)
add_library(GameNet::tls ALIAS gamenet_tls)
set_target_properties(gamenet_tls PROPERTIES EXPORT_NAME tls)
target_include_directories(gamenet_tls PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_tls PUBLIC cxx_std_23)
target_compile_options(gamenet_tls PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_tls PUBLIC GameNet::core OpenSSL::SSL)
gamenet_configure_sanitizers(gamenet_tls)
install(TARGETS gamenet_tls EXPORT GameNetTlsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/tls
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetTlsTargets NAMESPACE GameNet::
    DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
